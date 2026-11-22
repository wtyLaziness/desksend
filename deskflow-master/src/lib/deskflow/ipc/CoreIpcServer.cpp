/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025-2026 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "CoreIpcServer.h"

#include "base/Log.h"
#include "common/Constants.h"

#include "deskflow/FileDropBridge.h"

#include <QByteArray>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>

namespace deskflow::core::ipc {

static CoreIpcServer *s_instance = nullptr;

CoreIpcServer::CoreIpcServer(QObject *parent) : IpcServer(parent, kCoreIpcName, QStringLiteral("core"))
{
  assert(s_instance == nullptr);
  s_instance = this;
}

CoreIpcServer::~CoreIpcServer()
{
  if (s_instance == this) {
    s_instance = nullptr;
  }
}

CoreIpcServer &CoreIpcServer::instance()
{
  assert(s_instance != nullptr);
  return *s_instance;
}

CoreIpcServer *CoreIpcServer::instanceIfInitialized()
{
  return s_instance;
}

void CoreIpcServer::processCommand(QLocalSocket *clientSocket, const QString &command, const QStringList &parts)
{
  if (command == QStringLiteral("stop")) {
    LOG_DEBUG("core ipc server got stop message");
    writeToClientSocket(clientSocket, QStringLiteral("ok"));
    broadcastCommand(QStringLiteral("bye"));
    Q_EMIT stopProcessRequested();
    return;
  }
  if (command == QStringLiteral("getTransferPeers")) {
    const auto targets = FileDropBridge::instance().availableTransferTargets();
    const auto encoded = QString::fromStdString(FileDropBridge::encodeTransferTargetsForIpc(targets));
    writeToClientSocket(clientSocket, QStringLiteral("transferPeers=%1").arg(encoded));
    return;
  }
  if (command == QStringLiteral("sendFiles")) {
    if (parts.size() == 2 && parts.at(1).size() < 128 * 1024) {
      const auto decoded = QByteArray::fromBase64Encoding(parts.at(1).toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
      const auto json = decoded ? QJsonDocument::fromJson(decoded.decoded) : QJsonDocument{};
      if (json.isObject()) {
        const auto object = json.object();
        const auto target = object.value(QStringLiteral("target"));
        const auto jsonPaths = object.value(QStringLiteral("paths")).toArray();
        std::vector<std::string> paths;
        paths.reserve(static_cast<size_t>(jsonPaths.size()));
        for (const auto &value : jsonPaths) {
          const auto path = value.toString();
          const QFileInfo file(path);
          if (path.contains(QChar::Null) || !file.isAbsolute() || (!file.isFile() && !file.isDir())) {
            paths.clear();
            break;
          }
          paths.push_back(path.toStdString());
        }
        if (target.isString() && !target.toString().isEmpty() && !target.toString().contains(QChar::Null) &&
            !paths.empty() && FileDropBridge::instance().requestDirectFileSend(target.toString().toStdString(), paths)) {
          writeToClientSocket(clientSocket, QStringLiteral("ok"));
          return;
        }
      }
    }
    writeToClientSocket(clientSocket, QStringLiteral("error=sendFilesFailed"));
    return;
  }
  LOG_WARN("core ipc server got unknown command: %s", command.toUtf8().constData());
}

} // namespace deskflow::core::ipc
