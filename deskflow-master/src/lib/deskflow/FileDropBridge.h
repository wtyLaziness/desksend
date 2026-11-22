/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/DragOverlayWidgets.h"
#include "deskflow/DragProtocol.h"
#include "deskflow/DropExecutor.h"

#include <QPoint>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

class BaseClientProxy;
class FileDropBridgeTests;

namespace deskflow {

class FileDropBridge final : public IDropExecutorHost
{
public:
  using ClientDragMessageSender = std::function<void(uint32_t fileCount, const std::string &info)>;

  static FileDropBridge &instance();

  FileDropBridge(const FileDropBridge &) = delete;
  FileDropBridge &operator=(const FileDropBridge &) = delete;

  // --- Server-side (coordinator & primary computer) hooks ---
  void updatePrimaryShape(int32_t x, int32_t y, int32_t w, int32_t h);
  void setPrimaryEdgeSegments(std::vector<DragEdgeSegment> segments);
  void setServerClientProxies(const std::vector<BaseClientProxy *> &clients);
  std::vector<std::string> availableTransferTargets() const;
  static std::string encodeTransferTargetsForIpc(const std::vector<std::string> &targets);
  bool requestDirectFileSend(const std::string &targetPeer, const std::vector<std::string> &files);
  void setServerPeerHosts(const std::vector<std::string> &peerHosts);
  void shutdownSentinels();

  // Called on primary mouse motion; shows edge sentinel or updates primary hover capsule
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

  // Unified bidirectional coordinator hooks (nullptr screen == primary computer)
  bool canUnlockEdgeSwitchForDrag(BaseClientProxy *activeScreen) const;
  bool isDragActiveForSwitch(BaseClientProxy *fromScreen, BaseClientProxy *toScreen) const;
  void prepareScreenSwitch(BaseClientProxy *fromScreen, BaseClientProxy *toScreen);
  void onScreenSwitch(BaseClientProxy *fromScreen, BaseClientProxy *toScreen, int32_t entryX, int32_t entryY);
  void onServerReceiveClientDragInfo(BaseClientProxy *client, uint16_t fileCount, const std::string &info);
  void onServerLeftMouseUp(BaseClientProxy *activeScreen, int32_t x, int32_t y);

  // Legacy wrappers preserved for compatibility
  void onLeavePrimary(BaseClientProxy *dstClient);
  void onReturnToPrimary();

  // Called when a secondary client disconnects from Server to prevent dangling pointer
  void onClientDisconnected(BaseClientProxy *client);

  // Cancels any active drag session (e.g., on Esc key)
  void cancelActiveDrag();

  // Called when Server::onMouseUp(kButtonLeft) fires on a secondary computer
  void onSecondaryLeftMouseUp(BaseClientProxy *activeClient, int32_t x, int32_t y);

  // --- Client-side (endpoint computer) hooks ---
  void startReceiver(const std::string &serverHost);
  void startClientReceiver(const std::string &serverHost);
  void stopReceiver();
  void setClientDragMessageSender(ClientDragMessageSender sender);
  void onClientMouseLeftDown();
  void onClientMouseLeftUp();
  void onClientMouseMoveWithShape(int32_t x, int32_t y, int32_t ax, int32_t ay, int32_t aw, int32_t ah);
  void onClientLeave();
  void onClientReceiveDragInfo(uint16_t fileCount, const std::string &info);
  void onClientMouseMove(int32_t x, int32_t y);
  bool isClientHoverVisible() const;
  uint32_t clientHoverFileCount() const;
  QPoint clientHoverCursorPos() const;
  TransferStatusOverlay::Phase receiverOverlayPhase() const;
  uint32_t receiverPreparingFileCount() const;
  std::shared_ptr<TransferStatusOverlay> overlayFor(bool receiving);

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
  std::shared_ptr<InProcessDropExecutor> inProcessExecutorForTesting() const;

private:
  friend class ::FileDropBridgeTests;

  struct ActiveCrossDrag
  {
    bool active = false;
    BaseClientProxy *originClient = nullptr; // nullptr == primary computer
    BaseClientProxy *targetClient = nullptr; // nullptr == primary computer
    uint32_t fileCount = 0;
    std::vector<std::string> primaryFiles;
    std::string activeToken;
  };

  FileDropBridge();
  ~FileDropBridge() override;

  // IDropExecutorHost implementation
  void onPendingDragCaptured(std::vector<std::string> files, int sideIndex, int32_t x, int32_t y) override;
  void onDragCancellationCompleted(bool stillPressed) override;
  void onPendingDragCleared() override;
  void onActiveTransfersChanged(bool active) override;
  void onExecutorLog(int level, const std::string &message) override;

  std::shared_ptr<IDropExecutor> executor() const;
  void handleReceiveSessionStarted(size_t expectedFiles, const std::string &tokenTag = {});
  void handleReceiveSessionFinished(bool cancelled);
  void setDragCancellationState(bool inProgress, bool complete);
  void resetAllStateForTesting();

  void performScreenSwitch(
      BaseClientProxy *fromScreen, BaseClientProxy *toScreen, int32_t entryX, int32_t entryY, bool sendHoverEnter
  );
  void activateTargetScreen(
      const std::shared_ptr<IDropExecutor> &exec, BaseClientProxy *target, BaseClientProxy *origin, uint32_t fileCount,
      const std::vector<std::string> &primaryFiles, const std::string &token, int32_t entryX, int32_t entryY,
      bool sendHoverEnter
  ) const;
  void cancelTargetEndpoint(const std::shared_ptr<IDropExecutor> &exec, BaseClientProxy *target, const std::string &token, bool cancelPreparing) const;
  void refreshClientReceiverHosts();
  static std::string buildDragStartPayload(const std::string &token, const std::vector<std::string> &files);

  mutable std::mutex m_mutex;
  std::shared_ptr<IDropExecutor> m_executor;

  // Server/Primary pre-capture & sentinel state
  std::vector<std::string> m_pendingFiles;
  uint64_t m_pendingTimestampMs = 0;
  int m_lastCapturedSide = 0;
  bool m_canCaptureDrag = false;
  bool m_sourceButtonNeedsRelease = false;
  bool m_dragCancellationInProgress = false;
  bool m_dragCancellationComplete = false;
  bool m_primaryLeftButtonDown = false;
  uint32_t m_visibleSentinelEdgeMask = 0;
  std::vector<DragEdgeSegment> m_primaryEdgeSegments;
  bool m_primaryEdgeSegmentsConfigured = false;
  struct PendingDirectSend
  {
    std::string requestId;
    std::vector<std::string> files;
  };

  uint64_t m_nextDirectSendSeq = 0;
  std::vector<PendingDirectSend> m_clientDirectPending;
  std::vector<std::string> m_clientTransferTargets;
  std::vector<std::pair<std::string, BaseClientProxy *>> m_serverClients;
  std::vector<std::string> m_serverPeerHosts;

  // Server-side remote pre-capture & active cross-screen drag state
  BaseClientProxy *m_preCapturedClient = nullptr;
  uint16_t m_preCapturedClientFileCount = 0;
  DragEdgeSide m_preCapturedClientSide = DragEdgeSide::Left;
  uint64_t m_preCapturedClientTimestampMs = 0;
  ActiveCrossDrag m_activeDrag;

  // Client-side endpoint state
  ClientDragMessageSender m_clientDragSender;
  std::string m_clientServerHost;
  std::vector<std::string> m_clientPeerHosts;
  std::vector<DragEdgeSegment> m_clientEdgeSegments;
  std::vector<std::string> m_clientPendingFiles;
  uint64_t m_clientPendingTimestampMs = 0;
  int m_clientPendingSide = 0;
  bool m_clientCanCaptureDrag = false;
  bool m_clientLeftButtonDown = false;
  bool m_clientHandoffCommitted = false;
  bool m_clientCancellationInProgress = false;
  bool m_clientSentinelVisible = false;
};

} // namespace deskflow
