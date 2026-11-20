/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 DeskSend Contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/DragOverlayWidgets.h"
#include "deskflow/DropAgentIpc.h"

#include <QPoint>
#include <QString>

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

class FileDropBridgeTests;
class QProcess;

namespace deskflow {

class ReceiverRestartPolicy
{
public:
  static std::vector<std::string> normalizeHosts(const std::vector<std::string> &hosts);

  std::optional<std::vector<std::string>>
  requestHosts(const std::vector<std::string> &hosts, bool dragActive, bool transferActive);

  std::optional<std::vector<std::string>> onActivityChanged(bool dragActive, bool transferActive);

  bool hasDeferredRestart() const;
  const std::vector<std::string> &activeHosts() const;
  const std::vector<std::string> &deferredHosts() const;
  void reset();

private:
  std::vector<std::string> m_activeHosts;
  std::vector<std::string> m_deferredHosts;
  bool m_hasActive = false;
  bool m_hasDeferred = false;
};

class ReceiverAuthCache
{
public:
  struct Entry
  {
    std::string command;
    uint64_t timestampMs = 0;
  };

  static constexpr uint64_t kDefaultTtlMs = 300000;
  static constexpr size_t kMaxEntries = 64;

  void record(const std::string &command, uint64_t timestampMs, uint64_t ttlMs = kDefaultTtlMs);
  std::vector<std::string> unexpiredGrants(uint64_t timestampMs, uint64_t ttlMs = kDefaultTtlMs);
  void clear();

private:
  void pruneExpired(uint64_t timestampMs, uint64_t ttlMs);

  std::deque<Entry> m_entries;
};

class IDropExecutorHost
{
public:
  virtual ~IDropExecutorHost() = default;

  virtual void onExecutorReady() {}
  virtual void onPendingDragCaptured(std::vector<std::string> files, int sideIndex, int32_t x, int32_t y) = 0;
  virtual void onDragCancellationCompleted(bool stillPressed) = 0;
  virtual void onPendingDragCleared() = 0;
  virtual void onActiveTransfersChanged(bool active) = 0;
  virtual void onSendCompleted(const std::string &targetIp, bool success) {}
  virtual void onExecutorLog(int level, const std::string &message) {}
};

class IDropExecutor
{
public:
  virtual ~IDropExecutor() = default;

  virtual void setHost(IDropExecutorHost *host) = 0;
  virtual void execute(const DropAgentCommand &cmd) = 0;

  virtual bool isHoverVisible() const = 0;
  virtual uint32_t hoverFileCount() const = 0;
  virtual QPoint hoverCursorPos() const = 0;
  virtual TransferStatusOverlay::Phase receiverOverlayPhase() const = 0;
  virtual uint32_t receiverPreparingFileCount() const = 0;
  virtual TransferStatusOverlay &overlayFor(bool receiving) = 0;
  virtual bool hasActiveTransfers() const = 0;
};

class InProcessDropExecutor final : public IDropExecutor
{
public:
  struct SendJob
  {
    std::string targetIp;
    std::vector<std::string> files;
    std::string authToken;
  };

  struct DropTargetEntry
  {
    std::string dir;
    bool copyToClipboard = false;
    uint64_t timestampMs = 0;
    std::string tokenTag;
  };

  struct SentinelBridge;

  explicit InProcessDropExecutor(bool enableSentinels = true);
  ~InProcessDropExecutor() override;

  InProcessDropExecutor(const InProcessDropExecutor &) = delete;
  InProcessDropExecutor &operator=(const InProcessDropExecutor &) = delete;

  void setHost(IDropExecutorHost *host) override;
  void execute(const DropAgentCommand &cmd) override;

  bool isHoverVisible() const override;
  uint32_t hoverFileCount() const override;
  QPoint hoverCursorPos() const override;
  TransferStatusOverlay::Phase receiverOverlayPhase() const override;
  uint32_t receiverPreparingFileCount() const override;
  TransferStatusOverlay &overlayFor(bool receiving) override;
  bool hasActiveTransfers() const override;

  // Helpers exposed for FileDropBridge & unit tests
  void handleReceiveSessionStarted(
      size_t expectedFiles, const std::string &tokenTag = {}, const std::string &sessionId = {}
  );
  void handleReceiveSessionFinished(bool cancelled, const std::string &sessionId = {});
  void handleReceivedFileLine(const std::string &filePath);
  void handleReceiveFileFailedLine();

  static std::string resolveDropTargetDirectory(
      int32_t screenX, int32_t screenY, const std::string &fallbackDir, bool *outIsDirectFolderTarget = nullptr
  );
  static std::string uniqueDestinationPath(const std::string &dstDir, const std::string &relPath);
  static std::string findLocalSendCliPath();
  static std::string computeTokenTag(const std::string &token);

private:
  friend class ::FileDropBridgeTests;
  friend struct SentinelBridge;

  void updatePrimaryShape(int32_t x, int32_t y, int32_t w, int32_t h);
  void handlePrimaryMouseMove(
      int32_t x, int32_t y, int32_t ax, int32_t ay, int32_t aw, int32_t ah, uint32_t activeSides,
      const std::vector<DragEdgeSegment> &edgeSegments
  );
  void handlePrimaryLeftMouseDown();
  void handleLeftMouseUp(bool releaseSourceButton);
  void handleHideSentinels();
  void handleShutdownSentinels();
  void handleNudgeAcrossEdge(int sideIndex);
  void handleCancelNativeDrag(int sideIndex);

  void startReceiverHosts(const std::vector<std::string> &hosts);
  void startReceiverInternal(const std::vector<std::string> &hosts, bool isRestart);
  void stopReceiver();
  void queueReceiverAuthCommand(const std::string &command);
  void handleDropTarget(uint32_t fileCount, int32_t x, int32_t y, const std::string &tokenTag);
  void handleCancelDropToken(const std::string &tokenTag, const std::string &authToken);
  void sendFilesViaLocalSend(
      const std::string &targetIp, const std::vector<std::string> &files, const std::string &authToken
  );
  void processNextSendJob();
  void cancelActiveSend(QProcess *process);
  void cancelActiveReceive(const std::string &sessionId);
  void finalizeActiveSession();
  void notifyActiveTransfersChangedLocked(bool &outShouldNotify, bool &outActive);
  void emitActiveTransfersChanged(bool active);
  void maybeFlushDeferredRestart();

  void onSentinelDragEnter(int sideIndex, std::vector<std::string> files, int32_t x, int32_t y);
  void onSentinelDragLeave();
  void onSentinelCancellationCompleted(bool stillPressed);

  const bool m_enableSentinels;
  std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>>(true);
  std::atomic<IDropExecutorHost *> m_host{nullptr};
  std::unique_ptr<SentinelBridge> m_sentinels;

  mutable std::mutex m_mutex;
  ReceiverRestartPolicy m_restartPolicy;
  bool m_dragActive = false;

  std::deque<SendJob> m_sendQueue;
  bool m_sendingInProgress = false;
  bool m_sendCancelRequested = false;

  std::string m_defaultDropFallbackDir;
  std::string m_lastResolvedDropDir;
  std::deque<std::string> m_pendingReceiverAuthLines;
  ReceiverAuthCache m_cachedReceiverGrants;
  std::deque<DropTargetEntry> m_pendingDropTargets;
  DropTargetEntry m_activeSessionTarget;
  std::string m_activeReceiveSessionId;
  std::vector<std::string> m_activeSessionPlacedFiles;
  size_t m_activeSessionExpectedFiles = 0;
  size_t m_activeSessionHandledFiles = 0;
  bool m_activeSessionEnded = false;
  bool m_activeSessionFailed = false;
  bool m_activeSessionCancelled = false;
  bool m_activeReceiveSession = false;
  bool m_lastReportedActiveTransfers = false;

  DragHoverIndicator m_clientHoverIndicator{false};
  TransferStatusOverlay m_senderOverlay{false, false};
  TransferStatusOverlay m_receiverOverlay{true, false};

  static constexpr int kMaxConsecutiveReceiverRestarts = 5;

  std::string m_stagingDir;
  std::string m_currentAllowedHostsKey;
  bool m_receiverDesired = false;
  uint32_t m_receiverGeneration = 0;
  int m_receiverRestartAttempt = 0;
  QProcess *m_receiverProcess = nullptr;
  QProcess *m_activeSendProcess = nullptr;
};

#if defined(_WIN32)
bool isProcessRunningSystemOrElevated();
bool shouldUseOutOfProcessDropAgent();

class OutOfProcessDropExecutor final : public IDropExecutor
{
public:
  struct PlatformState;

  OutOfProcessDropExecutor();
  ~OutOfProcessDropExecutor() override;

  OutOfProcessDropExecutor(const OutOfProcessDropExecutor &) = delete;
  OutOfProcessDropExecutor &operator=(const OutOfProcessDropExecutor &) = delete;

  void setHost(IDropExecutorHost *host) override;
  void execute(const DropAgentCommand &cmd) override;

  bool isHoverVisible() const override;
  uint32_t hoverFileCount() const override;
  QPoint hoverCursorPos() const override;
  TransferStatusOverlay::Phase receiverOverlayPhase() const override;
  uint32_t receiverPreparingFileCount() const override;
  TransferStatusOverlay &overlayFor(bool receiving) override;
  bool hasActiveTransfers() const override;

private:
  void ensureAgentRunning();
  void sendOrQueueCommand(const DropAgentCommand &cmd);
  void writeCommandLocked(const DropAgentCommand &cmd);
  void scheduleReconnect(int delayMs);
  void readerLoop(uint32_t expectedGeneration);
  void handleAgentPipeClosed(uint32_t expectedGeneration);
  void handleAgentEvent(const DropAgentEvent &ev);
  void dispatchHostEvent(const DropAgentEvent &ev);
  void postToMainThread(std::function<void()> fn);
  void replayCachedStateLocked();

  std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>>(true);
  std::atomic<IDropExecutorHost *> m_host{nullptr};
  std::unique_ptr<PlatformState> m_platform;
  mutable std::mutex m_launchMutex;
  mutable std::mutex m_mutex;
  std::mutex m_writeMutex;
  bool m_shuttingDown = false;
  bool m_agentReady = false;
  bool m_reconnectScheduled = false;
  int m_restartAttempt = 0;
  uint32_t m_sessionGeneration = 0;

  bool m_hasPrimaryShape = false;
  int32_t m_shapeX = 0;
  int32_t m_shapeY = 0;
  int32_t m_shapeW = 0;
  int32_t m_shapeH = 0;

  bool m_dragActive = false;
  bool m_activeTransfers = false;
  bool m_receiverDesired = false;
  std::vector<std::string> m_receiverHosts;
  std::deque<std::string> m_cachedAuthLines;
  std::deque<DropAgentCommand> m_startupQueue;

  DragHoverIndicator m_shadowHoverIndicator{true};
  TransferStatusOverlay m_shadowSenderOverlay{false, true};
  TransferStatusOverlay m_shadowReceiverOverlay{true, true};
};
#endif

std::unique_ptr<IDropExecutor> createDefaultDropExecutor();

} // namespace deskflow
