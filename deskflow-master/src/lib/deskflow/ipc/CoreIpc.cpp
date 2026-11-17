/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "CoreIpc.h"

#include "CoreIpcServer.h"

#include <QMetaEnum>

void ipcSendToClient(const QString &command, const QString &args)
{
  auto *server = deskflow::core::ipc::CoreIpcServer::instanceIfInitialized();
  if (server == nullptr) {
    return;
  }
  // Queued because callers may not be on the main thread,
  // and QLocalSocket can only be written to from its owning thread.
  QMetaObject::invokeMethod(
      server,
      [command, args] {
        if (auto *current = deskflow::core::ipc::CoreIpcServer::instanceIfInitialized()) {
          current->broadcastCommand(command, args);
        }
      },
      Qt::QueuedConnection
  );
}

void ipcSendConnectionState(deskflow::core::ConnectionState state)
{
  const auto metaEnum = QMetaEnum::fromType<deskflow::core::ConnectionState>();
  ipcSendToClient(QStringLiteral("connectionState"), metaEnum.valueToKey(static_cast<int>(state)));
}
