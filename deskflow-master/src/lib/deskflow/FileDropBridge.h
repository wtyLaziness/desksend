/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

class BaseClientProxy;
class QProcess;

namespace deskflow {

class FileDropBridge
{
public:
  static FileDropBridge &instance();

  FileDropBridge(const FileDropBridge &) = delete;
  FileDropBridge &operator=(const FileDropBridge &) = delete;

  // --- Server-side (source computer) hooks ---
  void updatePrimaryShape(int32_t x, int32_t y, int32_t w, int32_t h);
  void shutdownSentinels();

  // Called on primary mouse motion; shows edge sentinel when dragging near an active border
  void onPrimaryMouseMove(
      int32_t x, int32_t y, int32_t ax, int32_t ay, int32_t aw, int32_t ah, uint32_t activeSides
  );

  // True when the edge sentinel has just captured CF_HDROP files while LBUTTON is held
  bool hasPendingDragFiles() const;

  // Called by sentinel DropTarget::DragEnter or by tests
  bool setPendingDragFiles(std::vector<std::string> files);
  void onPrimaryLeftMouseDown();
  bool canSwitchFromPrimary() const;

  // Called when Server::switchComputer leaves primary to a remote client
  void onLeavePrimary(BaseClientProxy *dstClient);

  // Called when Server::switchComputer returns back to primary (cancels unreleased drag)
  void onReturnToPrimary();

  // Called when a secondary client disconnects from Server to prevent dangling pointer
  void onClientDisconnected(BaseClientProxy *client);

  // Cancels any active drag session (e.g., on Esc key)
  void cancelActiveDrag();

  // Called when Server::onMouseUp(kButtonLeft) fires on a secondary computer
  void onSecondaryLeftMouseUp(BaseClientProxy *activeClient, int32_t x, int32_t y);

  // --- Client-side (destination computer) hooks ---
  void startReceiver(const std::string &serverHost);
  void stopReceiver();
  void onClientReceiveDragInfo(uint16_t fileCount, const std::string &info);

  // Resolves the Explorer folder or Desktop directory under (screenX, screenY) on Windows
  static std::string resolveDropTargetDirectory(
      int32_t screenX, int32_t screenY, const std::string &fallbackDir, bool *outIsDirectFolderTarget = nullptr
  );

  // Extracts the peer IP string from a connected client proxy
  static std::string getClientPeerIp(BaseClientProxy *client);

  // Locates localsend-cli binary next to current executable or via DESKSEND_LOCALSEND_CLI
  static std::string findLocalSendCliPath();

private:
  struct SendJob
  {
    std::string targetIp;
    std::vector<std::string> files;
  };

  struct DropTargetEntry
  {
    std::string dir;
    bool copyToClipboard = false;
    uint64_t timestampMs = 0;
  };

  FileDropBridge() = default;
  ~FileDropBridge();

  void sendFilesViaLocalSend(const std::string &targetIp, const std::vector<std::string> &files);
  void processNextSendJob();
  void handleReceivedFileLine(const std::string &filePath);
  void handleReceiveFileFailedLine();
  void handleReceiveSessionStarted(size_t expectedFiles);
  void handleReceiveSessionFinished(bool cancelled);
  void finalizeActiveSession();

  mutable std::mutex m_mutex;
  std::vector<std::string> m_pendingFiles;
  uint64_t m_pendingTimestampMs = 0;
  bool m_canCaptureDrag = false;

  std::vector<std::string> m_activeDragFiles;
  BaseClientProxy *m_dragTargetClient = nullptr;

  // Server-side serial send queue
  std::deque<SendJob> m_sendQueue;
  bool m_sendingInProgress = false;

  // Client-side drop target state & session queue
  std::string m_lastResolvedDropDir;
  std::deque<DropTargetEntry> m_pendingDropTargets;
  DropTargetEntry m_activeSessionTarget;
  std::vector<std::string> m_activeSessionPlacedFiles;
  size_t m_activeSessionExpectedFiles = 0;
  size_t m_activeSessionHandledFiles = 0;
  bool m_activeSessionEnded = false;
  bool m_activeSessionFailed = false;
  std::string m_stagingDir;
  std::string m_currentAllowedServerIp;
  QProcess *m_receiverProcess = nullptr;
};

} // namespace deskflow
