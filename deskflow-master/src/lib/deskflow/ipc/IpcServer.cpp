/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025-2026 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "IpcServer.h"

#include "base/Log.h"
#include "common/Constants.h"
#include "common/VersionInfo.h"

#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QTimer>
#include <utility>
#ifdef Q_OS_WIN
#include <QWinEventNotifier>
#include <qt_windows.h>
#include <sddl.h>
#endif

namespace deskflow::core::ipc {

#ifdef Q_OS_WIN
namespace {
bool isSystemProcess()
{
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    return false;
  alignas(TOKEN_USER) BYTE buffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
  DWORD size = 0;
  const bool isSystem = GetTokenInformation(token, TokenUser, buffer, sizeof(buffer), &size) &&
                        IsWellKnownSid(reinterpret_cast<TOKEN_USER *>(buffer)->User.Sid, WinLocalSystemSid);
  CloseHandle(token);
  return isSystem;
}

class ServicePipe final : public QObject
{
public:
  ServicePipe(QObject *parent, const QString &name)
      : QObject(parent), m_name(QStringLiteral(R"(\\.\pipe\)") + name)
  {
    m_operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  }

  ~ServicePipe() override
  {
    delete findChild<QWinEventNotifier *>();
    if (m_pipe != INVALID_HANDLE_VALUE) {
      CancelIoEx(m_pipe, &m_operation);
      DWORD transferred = 0;
      GetOverlappedResult(m_pipe, &m_operation, &transferred, TRUE);
      CloseHandle(m_pipe);
    }
    if (m_operation.hEvent)
      CloseHandle(m_operation.hEvent);
  }

  bool arm()
  {
    if (!m_operation.hEvent)
      return false;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;GA;;;SY)(A;;GA;;;BA)", SDDL_REVISION_1, &descriptor, nullptr))
      return false;
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
    const auto event = m_operation.hEvent;
    m_operation = {};
    m_operation.hEvent = event;
    ResetEvent(event);
    m_pipe = CreateNamedPipeW(
        reinterpret_cast<LPCWSTR>(m_name.utf16()),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        PIPE_UNLIMITED_INSTANCES, 0, 0, 3000, &attributes
    );
    LocalFree(descriptor);
    if (m_pipe == INVALID_HANDLE_VALUE)
      return false;
    m_connected = ConnectNamedPipe(m_pipe, &m_operation);
    if (!m_connected) {
      const auto error = GetLastError();
      if (error == ERROR_IO_PENDING)
        return true;
      if (error != ERROR_PIPE_CONNECTED) {
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
        return false;
      }
      m_connected = true;
    }
    SetEvent(event);
    return true;
  }

  HANDLE event() const { return m_operation.hEvent; }

  HANDLE takeConnection()
  {
    DWORD transferred = 0;
    if (!m_connected && !GetOverlappedResult(m_pipe, &m_operation, &transferred, FALSE))
      return INVALID_HANDLE_VALUE;
    return std::exchange(m_pipe, INVALID_HANDLE_VALUE);
  }

private:
  QString m_name;
  OVERLAPPED m_operation{};
  HANDLE m_pipe = INVALID_HANDLE_VALUE;
  bool m_connected = false;
};
} // namespace
#endif

IpcServer::IpcServer(QObject *parent, const QString &serverName, const QString &typeName)
    : QObject(parent),
      m_server{new QLocalServer(this)}, // NOSONAR - Qt memory
      m_serverName(serverName),
      m_typeName(typeName.toUtf8())
{
  // do nothing
}

IpcServer::~IpcServer()
{
  m_server->close();
}

void IpcServer::listen(bool allowUnprivilegedClients)
{
  Q_UNUSED(allowUnprivilegedClients);
#ifdef Q_OS_WIN
  if (isSystemProcess()) {
    auto *listener = new ServicePipe(this, m_serverName);
    if (!listener->arm()) {
      LOG_ERR("could not listen on the DeskSend service pipe");
      delete listener;
      return;
    }
    auto *notifier = new QWinEventNotifier(listener->event(), listener);
    connect(notifier, &QWinEventNotifier::activated, this, [this, listener, notifier] {
      const auto pipe = listener->takeConnection();
      if (pipe == INVALID_HANDLE_VALUE)
        return;
      if (!listener->arm()) {
        notifier->setEnabled(false);
        LOG_ERR("could not rearm the DeskSend service pipe");
      }
      auto *socket = new QLocalSocket(this);
      if (socket->setSocketDescriptor(reinterpret_cast<qintptr>(pipe)))
        acceptClient(socket);
      else {
        CloseHandle(pipe);
        delete socket;
      }
    });
    return;
  }
#endif
  m_server->setSocketOptions(QLocalServer::UserAccessOption);

  connect(m_server, &QLocalServer::newConnection, this, &IpcServer::handleNewConnection);
  QLocalServer::removeServer(m_serverName);
  if (m_server->listen(m_serverName)) {
    LOG_DEBUG("%s ipc server listening on: %s", m_typeName.constData(), m_serverName.toUtf8().constData());
  } else {
    LOG_ERR("%s ipc server failed to listen on: %s", m_typeName.constData(), m_serverName.toUtf8().constData());
  }
}

void IpcServer::handleNewConnection()
{
  auto *clientSocket = m_server->nextPendingConnection();
  if (clientSocket)
    acceptClient(clientSocket);
}

void IpcServer::acceptClient(QLocalSocket *clientSocket)
{
  if (m_clients.size() >= 16) {
    clientSocket->abort();
    clientSocket->deleteLater();
    return;
  }
  auto *timer = new QTimer(clientSocket);
  timer->setSingleShot(true);
  timer->setInterval(5000);
  connect(timer, &QTimer::timeout, clientSocket, &QLocalSocket::abort);
  timer->start();

  LOG_DEBUG("%s ipc server got new connection", m_typeName.constData());
  m_clients.insert(clientSocket);
  clientSocket->setReadBufferSize(kMaxIpcCommandBytes);

  connect(clientSocket, &QLocalSocket::readyRead, this, &IpcServer::handleReadyRead);
  connect(clientSocket, &QLocalSocket::disconnected, this, &IpcServer::handleDisconnected);
  connect(clientSocket, &QLocalSocket::errorOccurred, this, &IpcServer::handleErrorOccurred);
  if (clientSocket->bytesAvailable())
    QMetaObject::invokeMethod(clientSocket, [clientSocket] { Q_EMIT clientSocket->readyRead(); }, Qt::QueuedConnection);
}

void IpcServer::handleReadyRead()
{
  const QPointer<QLocalSocket> clientSocket(qobject_cast<QLocalSocket *>(sender()));
  const auto isConnected = [&] {
    return clientSocket && m_clients.contains(clientSocket.data()) &&
           clientSocket->state() == QLocalSocket::ConnectedState;
  };
  LOG_VERBOSE("%s ipc server ready to read data", m_typeName.constData());

  // 只取出完整行，留在套接字缓冲区中的半条命令会等下次读取。
  while (isConnected() && clientSocket->canReadLine()) {
    const auto messageData = clientSocket->readLine();
    const auto message = QString::fromUtf8(messageData.constData(), messageData.size() - 1);
    processMessage(clientSocket.data(), message);
  }
  if (!isConnected())
    return;
  if (m_readyClients.contains(clientSocket.data())) {
    if (auto *timer = clientSocket->findChild<QTimer *>()) {
      if (clientSocket->bytesAvailable())
        timer->start();
      else
        timer->stop();
    }
  }
  if (clientSocket->bytesAvailable() >= kMaxIpcCommandBytes) {
    LOG_WARN("%s ipc command exceeds the size limit", m_typeName.constData());
    clientSocket->abort();
  }
}

void IpcServer::handleDisconnected()
{
  const auto clientSocket = qobject_cast<QLocalSocket *>(sender());
  LOG_DEBUG("%s ipc server client disconnected", m_typeName.constData());
  m_clients.remove(clientSocket);
  m_readyClients.remove(clientSocket);
  clientSocket->deleteLater();
}

void IpcServer::handleErrorOccurred()
{
  const auto clientSocket = qobject_cast<QLocalSocket *>(sender());
  if (clientSocket->error() == QLocalSocket::PeerClosedError) {
    LOG_DEBUG("%s ipc server client closed connection", m_typeName.constData());
  } else {
    LOG_ERR("%s ipc server client error: %s", m_typeName.constData(), clientSocket->errorString().toUtf8().constData());
  }

  m_clients.remove(clientSocket);
  m_readyClients.remove(clientSocket);
  clientSocket->deleteLater();
}

void IpcServer::processMessage(QLocalSocket *clientSocket, const QString &message)
{
  LOG_VERBOSE("%s ipc server got message: %s", m_typeName.constData(), message.toUtf8().constData());
  if (!m_readyClients.contains(clientSocket) && !message.startsWith(QStringLiteral("hello="))) {
    clientSocket->abort();
    return;
  }
  if (message.isEmpty()) {
    LOG_ERR("%s ipc server got invalid message: %s", m_typeName.constData(), message.toUtf8().constData());
    writeToClientSocket(clientSocket, QStringLiteral("error"));
    return;
  }

  const auto separator = message.indexOf('=');
  const auto parts = separator < 0 ? QStringList{message}
                                  : QStringList{message.left(separator), message.mid(separator + 1)};
  if (const auto &command = parts.at(0); command == QStringLiteral("hello")) {
    if (m_readyClients.contains(clientSocket) || parts.size() != 2 || parts.at(1).isEmpty()) {
      LOG_ERR("%s ipc client hello missing version", m_typeName.constData());
      writeToClientSocket(clientSocket, "error=missing version");
      clientSocket->flush();
      clientSocket->disconnectFromServer();
      return;
    }

    const auto versionId = QStringLiteral("%1+%2").arg(kVersion, kVersionGitSha);
    const auto clientVersion = parts.at(1);
    LOG_DEBUG("%s ipc server got hello message (version: %s)", m_typeName.constData(), versionId.toUtf8().constData());

    if (clientVersion != versionId) {
      LOG_WARN(
          "%s ipc client version mismatch (client: %s, server: %s)", m_typeName.constData(),
          clientVersion.toUtf8().constData(), versionId.toUtf8().constData()
      );
      writeToClientSocket(clientSocket, QStringLiteral("versionMismatch=%1").arg(versionId));
      clientSocket->disconnectFromServer();
      return;
    }

    m_readyClients.insert(clientSocket);
    LOG_DEBUG("%s ipc server sending hello back", m_typeName.constData());
    writeToClientSocket(clientSocket, QStringLiteral("hello=%1").arg(versionId));

    // Replay messages that were queued before any clients connected.
    LOG_VERBOSE("ipc server replaying %d pending messages", m_pendingMessages.size());
    for (const auto &pending : std::as_const(m_pendingMessages)) {
      LOG_VERBOSE("%s ipc server replaying: %s", m_typeName.constData(), pending.toUtf8().constData());
      writeToClientSocket(clientSocket, pending);
    }
    m_pendingMessages.clear();
  } else if (command == QStringLiteral("noop")) {
    LOG_DEBUG("%s ipc server got noop message", m_typeName.constData());
    writeToClientSocket(clientSocket, QStringLiteral("ok"));
  } else {
    processCommand(clientSocket, command, parts);
  }

  clientSocket->flush();
}

void IpcServer::broadcastCommand(const QString &command, const QString &args)
{
  const auto message = args.isEmpty() ? command : QStringLiteral("%1=%2").arg(command, args);

  if (message.toUtf8().size() >= kMaxIpcCommandBytes)
    return;
  if (m_readyClients.isEmpty()) {
    LOG_VERBOSE(
        "%s ipc server has no clients, message queued: %s", m_typeName.constData(), message.toUtf8().constData()
    );
    qsizetype queuedBytes = message.toUtf8().size() + 1;
    for (const auto &pending : std::as_const(m_pendingMessages))
      queuedBytes += pending.toUtf8().size() + 1;
    while (m_pendingMessages.size() >= 64 || queuedBytes > kMaxIpcCommandBytes) {
      queuedBytes -= m_pendingMessages.takeFirst().toUtf8().size() + 1;
    }
    m_pendingMessages.append(message);
    return;
  }

  LOG_VERBOSE(
      "%s ipc server broadcasting message to %d clients: %s", m_typeName.constData(), m_clients.size(),
      message.toUtf8().constData()
  );
  const auto clients = m_readyClients;
  for (auto *client : clients) {
    writeToClientSocket(client, message);
    client->flush();
  }
}

void IpcServer::writeToClientSocket(QLocalSocket *&clientSocket, const QString &message) const
{
  QByteArray messageData = message.toUtf8() + '\n';
  if (messageData.size() > kMaxIpcCommandBytes || clientSocket->bytesToWrite() > 512 * 1024 - messageData.size()) {
    clientSocket->abort();
    return;
  }
  qint64 bytesWritten = clientSocket->write(messageData);
  if (bytesWritten != messageData.size()) {
    LOG_ERR("%s ipc server failed to write full message to client socket", m_typeName.constData());
  } else {
    LOG_VERBOSE(
        "%s ipc server wrote message to client socket: %s", m_typeName.constData(), message.toUtf8().constData()
    );
  }
}

} // namespace deskflow::core::ipc
