/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/DragOverlayWidgets.h"
#include "deskflow/DropExecutor.h"

#include <QPoint>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class BaseClientProxy;
class FileDropBridgeTests;

namespace deskflow {

class FileDropBridge final : public IDropExecutorHost
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
  bool hasActiveDragFiles() const;
  bool hasActiveTransfers() const;

  // Called by sentinel DropTarget::DragEnter or by tests
  static bool isSentinelCaptureSupportedFormat(bool hasHdrop, bool nonEmpty);
  void clearPendingDragOnMouseLeaveOrRelease();
  bool isDragHandoffInProgress() const;
  bool setPendingDragFiles(std::vector<std::string> files);
  void onPrimaryLeftMouseDown();
  void onLeftMouseUp(bool onSecondary);
  bool canSwitchFromPrimary();

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
  void onClientMouseMove(int32_t x, int32_t y);
  bool isClientHoverVisible() const;
  uint32_t clientHoverFileCount() const;
  QPoint clientHoverCursorPos() const;
  TransferStatusOverlay::Phase receiverOverlayPhase() const;
  uint32_t receiverPreparingFileCount() const;
  TransferStatusOverlay &overlayFor(bool receiving);

  // Resolves the Explorer folder or Desktop directory under (screenX, screenY) on Windows
  static std::string resolveDropTargetDirectory(
      int32_t screenX, int32_t screenY, const std::string &fallbackDir, bool *outIsDirectFolderTarget = nullptr
  );

  // Computes a collision-free destination file path inside dstDir (preserving dotfiles such as .gitignore)
  static std::string uniqueDestinationPath(const std::string &dstDir, const std::string &relPath);

  // Extracts the peer IP string from a connected client proxy
  static std::string getClientPeerIp(BaseClientProxy *client);

  // Locates localsend-cli binary next to current executable or via DESKSEND_LOCALSEND_CLI
  static std::string findLocalSendCliPath();

  // Computes a 16-hex-char SHA-256 correlation tag for a transfer auth token
  static std::string computeTokenTag(const std::string &token);

  // Testing hooks
  void setDropExecutorForTesting(std::unique_ptr<IDropExecutor> executor);
  InProcessDropExecutor *inProcessExecutorForTesting() const;

private:
  friend class ::FileDropBridgeTests;

  FileDropBridge();
  ~FileDropBridge() override;

  // IDropExecutorHost implementation
  void onPendingDragCaptured(std::vector<std::string> files, int sideIndex, int32_t x, int32_t y) override;
  void onDragCancellationCompleted(bool stillPressed) override;
  void onPendingDragCleared() override;
  void onActiveTransfersChanged(bool active) override;
  void onExecutorLog(int level, const std::string &message) override;

  IDropExecutor *executor() const;
  void handleReceiveSessionStarted(size_t expectedFiles, const std::string &tokenTag = {});
  void handleReceiveSessionFinished(bool cancelled);
  void setDragCancellationState(bool inProgress, bool complete);

  mutable std::mutex m_mutex;
  std::unique_ptr<IDropExecutor> m_executor;

  std::vector<std::string> m_pendingFiles;
  uint64_t m_pendingTimestampMs = 0;
  int m_lastCapturedSide = 0;
  bool m_canCaptureDrag = false;
  bool m_sourceButtonNeedsRelease = false;
  bool m_dragCancellationInProgress = false;
  bool m_dragCancellationComplete = false;
  uint32_t m_visibleSentinelEdgeMask = 0;

  std::vector<std::string> m_activeDragFiles;
  std::string m_activeDragToken;
  BaseClientProxy *m_dragTargetClient = nullptr;
};

} // namespace deskflow
