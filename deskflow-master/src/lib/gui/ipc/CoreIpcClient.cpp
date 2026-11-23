/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025-2026 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "CoreIpcClient.h"

#include "common/Constants.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

namespace deskflow::gui::ipc {

CoreIpcClient::CoreIpcClient(QObject *parent) : IpcClient(parent, kCoreIpcName, QStringLiteral("core"))
{
  // do nothing
}

void CoreIpcClient::sendStop()
{
  sendMessage(QStringLiteral("stop"));
}

void CoreIpcClient::requestTransferPeers()
{
  if (isConnected()) {
    sendMessage(QStringLiteral("getTransferPeers"));
  }
}

bool CoreIpcClient::sendFiles(const QString &targetPeer, const QStringList &paths)
{
  QStringList cleanedPaths;
  for (const auto &path : paths) {
    if (!path.isEmpty()) {
      cleanedPaths.append(path);
    }
  }
  if (cleanedPaths.isEmpty() || !isConnected()) {
    return false;
  }
  const QJsonObject payload{
      {QStringLiteral("target"), targetPeer},
      {QStringLiteral("paths"), QJsonArray::fromStringList(cleanedPaths)},
  };
  const auto encoded = QJsonDocument(payload).toJson(QJsonDocument::Compact).toBase64();
  return sendMessage(QStringLiteral("sendFiles=%1").arg(QString::fromLatin1(encoded)));
}

void CoreIpcClient::processCommand(const QString &command, const QStringList &parts)
{
  const auto args = parts.size() >= 2 ? parts.at(1) : QString();
  Q_EMIT commandReceived(command, args);
}

} // namespace deskflow::gui::ipc
