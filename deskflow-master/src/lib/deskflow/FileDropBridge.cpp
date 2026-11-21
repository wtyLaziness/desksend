/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "deskflow/FileDropBridge.h"

#include "base/DirectionTypes.h"
#include "base/Log.h"
#include "deskflow/DragProtocol.h"
#include "deskflow/PlatformDropSupport.h"
#include "deskflow/ipc/CoreIpc.h"
#include "io/StreamFilter.h"
#include "net/IDataSocket.h"
#include "server/BaseClientProxy.h"

#include <QByteArray>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QString>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <optional>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace deskflow {

namespace {

std::vector<std::string> serverTransferTargets(const std::vector<std::pair<std::string, BaseClientProxy *>> &clients)
{
  std::vector<std::string> targets;
  targets.reserve(clients.size());
  for (const auto &entry : clients) {
    targets.push_back(entry.first);
  }
  return targets;
}

void notifyTransferTargetsChanged(const std::vector<std::string> &targets)
{
  ipcSendToClient(QStringLiteral("transferPeers"), QString::fromStdString(FileDropBridge::encodeTransferTargetsForIpc(targets)));
}

uint64_t nowMs()
{
  using namespace std::chrono;
  return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

std::string generateAuthToken()
{
  std::array<quint32, 8> words{};
  QRandomGenerator::system()->fillRange(words.data(), static_cast<qsizetype>(words.size()));
  const QByteArray raw(
      reinterpret_cast<const char *>(words.data()), static_cast<qsizetype>(words.size() * sizeof(quint32))
  );
  return raw.toHex().toStdString();
}

bool hasTransferableFiles(const std::vector<std::string> &paths)
{
  for (const auto &p : paths) {
    const QFileInfo info(QString::fromStdString(p));
    if (info.isFile()) {
      return true;
    }
    if (info.isDir()) {
      QDirIterator it(
          info.absoluteFilePath(), QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories
      );
      if (it.hasNext()) {
        return true;
      }
    }
  }
  return false;
}

uint32_t computeNearEdgeMask(
    int32_t x, int32_t y, int32_t ax, int32_t ay, int32_t aw, int32_t ah, uint32_t activeSides
)
{
  using enum DirectionMask;
  constexpr int32_t zone = 16;
  const bool nearLeft = ((activeSides & static_cast<uint32_t>(LeftMask)) != 0) && (x < ax + zone);
  const bool nearRight = ((activeSides & static_cast<uint32_t>(RightMask)) != 0) && (x >= ax + aw - zone);
  const bool nearTop = ((activeSides & static_cast<uint32_t>(TopMask)) != 0) && (y < ay + zone);
  const bool nearBottom = ((activeSides & static_cast<uint32_t>(BottomMask)) != 0) && (y >= ay + ah - zone);

  uint32_t mask = 0;
  if (nearLeft) {
    mask |= static_cast<uint32_t>(LeftMask);
  }
  if (nearRight) {
    mask |= static_cast<uint32_t>(RightMask);
  }
  if (nearTop) {
    mask |= static_cast<uint32_t>(TopMask);
  }
  if (nearBottom) {
    mask |= static_cast<uint32_t>(BottomMask);
  }
  return mask;
}

uint32_t activeSidesMaskFromSegments(const std::vector<DragEdgeSegment> &segments)
{
  uint32_t mask = 0;
  for (const auto &seg : segments) {
    mask |= static_cast<uint32_t>(dragEdgeSideToDirectionMask(seg.side));
  }
  return mask;
}

void applyReceiverHosts(IDropExecutor *exec, const std::vector<std::string> &hosts)
{
  if (!exec) {
    return;
  }
  const auto normalized = ReceiverRestartPolicy::normalizeHosts(hosts);
  DropAgentCommand cmd;
  if (normalized.empty()) {
    cmd.type = DropAgentCommandType::StopReceiver;
  } else {
    cmd.type = DropAgentCommandType::StartReceiver;
    cmd.items = normalized;
  }
  exec->execute(cmd);
}

void sendOriginCancelMessage(BaseClientProxy *origin, const std::string &token)
{
  if (!origin) {
    return;
  }
  const std::string msg = serializeSendCancelMessage(token);
  origin->sendDragInfo(0, msg.data(), msg.size());
}

} // namespace

FileDropBridge &FileDropBridge::instance()
{
  static FileDropBridge s_instance;
  return s_instance;
}

FileDropBridge::FileDropBridge()
    : m_executor(createDefaultDropExecutor())
{
  if (m_executor) {
    m_executor->setHost(this);
  }
}

FileDropBridge::~FileDropBridge()
{
  std::unique_ptr<IDropExecutor> exec;
  {
    std::scoped_lock lock(m_mutex);
    exec = std::move(m_executor);
  }
  if (exec) {
    exec->setHost(nullptr);
    DropAgentCommand cmd;
    cmd.type = DropAgentCommandType::Shutdown;
    exec->execute(cmd);
  }
}

IDropExecutor *FileDropBridge::executor() const
{
  std::scoped_lock lock(m_mutex);
  return m_executor.get();
}

void FileDropBridge::setDropExecutorForTesting(std::unique_ptr<IDropExecutor> executor)
{
  std::unique_ptr<IDropExecutor> oldExec;
  {
    std::scoped_lock lock(m_mutex);
    oldExec = std::move(m_executor);
    m_executor = executor ? std::move(executor) : createDefaultDropExecutor();
    if (m_executor) {
      m_executor->setHost(this);
    }
  }
  if (oldExec) {
    oldExec->setHost(nullptr);
    DropAgentCommand cmd;
    cmd.type = DropAgentCommandType::Shutdown;
    oldExec->execute(cmd);
  }
}

InProcessDropExecutor *FileDropBridge::inProcessExecutorForTesting() const
{
  return dynamic_cast<InProcessDropExecutor *>(executor());
}

void FileDropBridge::updatePrimaryShape(int32_t x, int32_t y, int32_t w, int32_t h)
{
  if (auto *exec = executor()) {
    DropAgentCommand cmd;
    cmd.type = DropAgentCommandType::UpdatePrimaryShape;
    cmd.x = x;
    cmd.y = y;
    cmd.ax = x;
    cmd.ay = y;
    cmd.aw = w;
    cmd.ah = h;
    exec->execute(cmd);
  }
}

void FileDropBridge::setPrimaryEdgeSegments(std::vector<DragEdgeSegment> segments)
{
  std::scoped_lock lock(m_mutex);
  m_primaryEdgeSegments = std::move(segments);
  m_primaryEdgeSegmentsConfigured = true;
}

void FileDropBridge::setServerClientProxies(const std::vector<BaseClientProxy *> &clients)
{
  std::optional<std::vector<std::string>> targets;
  {
    std::scoped_lock lock(m_mutex);
    m_serverClients.clear();
    for (auto *client : clients) {
      if (client && !client->getName().empty()) {
        m_serverClients.emplace_back(client->getName(), client);
      }
    }
    if (m_clientServerHost.empty()) {
      targets = serverTransferTargets(m_serverClients);
    }
  }
  if (targets) {
    notifyTransferTargetsChanged(*targets);
  }
}

std::vector<std::string> FileDropBridge::availableTransferTargets() const
{
  std::scoped_lock lock(m_mutex);
  if (!m_clientServerHost.empty()) {
    return m_clientTransferTargets;
  }
  return serverTransferTargets(m_serverClients);
}

std::string FileDropBridge::encodeTransferTargetsForIpc(const std::vector<std::string> &targets)
{
  QJsonArray array;
  for (const auto &target : targets) {
    array.append(QString::fromStdString(target));
  }
  return QJsonDocument(array).toJson(QJsonDocument::Compact).toBase64().toStdString();
}

bool FileDropBridge::requestDirectFileSend(const std::string &targetPeer, const std::vector<std::string> &files)
{
  if (files.empty() || files.size() > UINT16_MAX || !hasTransferableFiles(files)) {
    return false;
  }

  ClientDragMessageSender sender;
  std::string request;
  BaseClientProxy *target = nullptr;
  {
    std::scoped_lock lock(m_mutex);
    if (m_clientDragSender && !m_clientServerHost.empty()) {
      if (m_clientDirectPending.size() >= 64) {
        return false;
      }
      const auto requestId = "d" + std::to_string(++m_nextDirectSendSeq);
      request = serializeDirectSendRequestMessage(requestId, targetPeer.empty() ? kServerPeerTargetMarker : targetPeer);
      if (request.empty()) {
        return false;
      }
      sender = m_clientDragSender;
      m_clientDirectPending.push_back({requestId, files});
    } else if (targetPeer.empty() && m_serverClients.size() == 1) {
      target = m_serverClients.front().second;
    } else {
      for (const auto &[name, proxy] : m_serverClients) {
        if (name == targetPeer) {
          target = proxy;
          break;
        }
      }
    }
  }
  const auto fileCount = static_cast<uint16_t>(files.size());
  if (sender) {
    sender(fileCount, request);
    return true;
  }

  auto *exec = executor();
  if (!target || !exec) {
    return false;
  }
  const auto peerIp = getClientPeerIp(target);
  if (peerIp.empty()) {
    return false;
  }

  const auto token = generateAuthToken();
  const auto start = buildDragStartPayload(token, files);
  const auto drop = serializeDragDropMessage(kDirectSendDropCoord, kDirectSendDropCoord, computeTokenTag(token));
  target->sendDragInfo(fileCount, start.data(), start.size());
  target->sendDragInfo(fileCount, drop.data(), drop.size());
  DropAgentCommand cmd;
  cmd.type = DropAgentCommandType::SendFiles;
  cmd.text = peerIp;
  cmd.items = files;
  cmd.authToken = token;
  exec->execute(cmd);
  return true;
}

void FileDropBridge::setServerPeerHosts(const std::vector<std::string> &peerHosts)
{
  bool clearedClients = false;
  {
    std::scoped_lock lock(m_mutex);
    m_serverPeerHosts = peerHosts;
    if (peerHosts.empty() && !m_serverClients.empty()) {
      m_serverClients.clear();
      clearedClients = true;
    }
  }
  if (clearedClients) {
    notifyTransferTargetsChanged({});
  }
  applyReceiverHosts(executor(), peerHosts);
}

void FileDropBridge::resetAllStateForTesting()
{
  std::scoped_lock lock(m_mutex);
  m_clientDirectPending.clear();
  m_clientTransferTargets.clear();
  m_serverClients.clear();
  m_pendingFiles.clear();
  m_pendingTimestampMs = 0;
  m_lastCapturedSide = 0;
  m_canCaptureDrag = false;
  m_sourceButtonNeedsRelease = false;
  m_dragCancellationInProgress = false;
  m_dragCancellationComplete = false;
  m_visibleSentinelEdgeMask = 0;
  m_primaryEdgeSegments.clear();
  m_primaryEdgeSegmentsConfigured = false;
  m_serverPeerHosts.clear();

  m_preCapturedClient = nullptr;
  m_preCapturedClientFileCount = 0;
  m_preCapturedClientSide = DragEdgeSide::Left;
  m_preCapturedClientTimestampMs = 0;
  m_activeDrag = {};

  m_clientDragSender = {};
  m_clientServerHost.clear();
  m_clientPeerHosts.clear();
  m_clientEdgeSegments.clear();
  m_clientPendingFiles.clear();
  m_clientPendingTimestampMs = 0;
  m_clientPendingSide = 0;
  m_clientCanCaptureDrag = false;
  m_clientHandoffCommitted = false;
  m_clientCancellationInProgress = false;
  m_clientSentinelVisible = false;
}

void FileDropBridge::shutdownSentinels()
{
  {
    std::scoped_lock lock(m_mutex);
    m_pendingFiles.clear();
    m_canCaptureDrag = false;
    m_sourceButtonNeedsRelease = false;
    m_dragCancellationInProgress = false;
    m_dragCancellationComplete = false;
    m_visibleSentinelEdgeMask = 0;
    m_primaryEdgeSegments.clear();
    m_primaryEdgeSegmentsConfigured = false;
    m_preCapturedClient = nullptr;
    m_preCapturedClientFileCount = 0;
    m_preCapturedClientTimestampMs = 0;
    m_activeDrag = {};
  }
  if (auto *exec = executor()) {
    DropAgentCommand cmd;
    cmd.type = DropAgentCommandType::ShutdownSentinels;
    exec->execute(cmd);
  }
}

void FileDropBridge::onPrimaryMouseMove(
    int32_t x, int32_t y, int32_t ax, int32_t ay, int32_t aw, int32_t ah, uint32_t activeSides
)
{
  bool hoveringOnPrimary = false;
  bool shouldHideForHover = false;
  bool segmentsConfigured = false;
#if !defined(_WIN32)
  bool trackedDown = false;
#endif
  std::vector<DragEdgeSegment> primarySegments;
  {
    std::scoped_lock lock(m_mutex);
#if !defined(_WIN32)
    trackedDown = m_primaryLeftButtonDown;
#endif
    hoveringOnPrimary = m_activeDrag.active && (m_activeDrag.targetClient == nullptr);
    if (hoveringOnPrimary) {
      shouldHideForHover = (m_visibleSentinelEdgeMask != 0);
      m_visibleSentinelEdgeMask = 0;
    } else {
      segmentsConfigured = m_primaryEdgeSegmentsConfigured;
      primarySegments = m_primaryEdgeSegments;
    }
  }

  if (hoveringOnPrimary) {
    if (auto *exec = executor()) {
      if (shouldHideForHover) {
        DropAgentCommand hideCmd;
        hideCmd.type = DropAgentCommandType::HideSentinels;
        exec->execute(hideCmd);
      }
      DropAgentCommand hoverCmd;
      hoverCmd.type = DropAgentCommandType::UpdateHoverCursor;
      hoverCmd.x = x;
      hoverCmd.y = y;
      exec->execute(hoverCmd);
    }
    return;
  }

#if defined(_WIN32)
  const bool leftButtonDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
#else
  const bool sentinelSupported = platform_drop::isNativeEdgeSentinelSupported();
  const bool leftButtonDown = sentinelSupported && platform_drop::isNativeLeftMouseButtonDown(trackedDown);
#endif
  if (!leftButtonDown) {
    bool shouldHide = false;
    {
      std::scoped_lock lock(m_mutex);
      shouldHide = (m_visibleSentinelEdgeMask != 0);
      m_visibleSentinelEdgeMask = 0;
      if (!m_pendingFiles.empty() && (nowMs() - m_pendingTimestampMs > 500)) {
#if !defined(_WIN32)
        shouldHide = shouldHide || !sentinelSupported;
#endif
        m_pendingFiles.clear();
      }
    }
    if (shouldHide) {
      if (auto *exec = executor()) {
        DropAgentCommand cmd;
        cmd.type = DropAgentCommandType::HideSentinels;
        exec->execute(cmd);
      }
    }
    return;
  }

  const uint32_t effectiveActiveSides =
      segmentsConfigured ? (primarySegments.empty() ? 0u : activeSidesMaskFromSegments(primarySegments)) : activeSides;
  const uint32_t visibleMask = computeNearEdgeMask(x, y, ax, ay, aw, ah, effectiveActiveSides);
  {
    std::scoped_lock lock(m_mutex);
    m_visibleSentinelEdgeMask = visibleMask;
  }
  if (auto *exec = executor()) {
    DropAgentCommand cmd;
    cmd.type = DropAgentCommandType::PrimaryMouseMove;
    cmd.x = x;
    cmd.y = y;
    cmd.ax = ax;
    cmd.ay = ay;
    cmd.aw = aw;
    cmd.ah = ah;
    cmd.activeSides = effectiveActiveSides;
    cmd.edgeSegments = std::move(primarySegments);
    exec->execute(cmd);
  }
}

bool FileDropBridge::isSentinelCaptureSupportedFormat(bool hasHdrop, bool nonEmpty)
{
  return hasHdrop && nonEmpty;
}

void FileDropBridge::clearPendingDragOnMouseLeaveOrRelease()
{
  {
    std::scoped_lock lock(m_mutex);
    m_primaryLeftButtonDown = false;
    m_pendingFiles.clear();
    m_canCaptureDrag = false;
    m_dragCancellationInProgress = false;
    m_dragCancellationComplete = false;
    m_visibleSentinelEdgeMask = 0;
  }
  if (auto *exec = executor()) {
    DropAgentCommand cmd;
    cmd.type = DropAgentCommandType::LeftMouseUp;
    cmd.flag = false;
    exec->execute(cmd);
  }
}

bool FileDropBridge::isDragHandoffInProgress() const
{
  std::scoped_lock lock(m_mutex);
  return m_dragCancellationInProgress;
}

void FileDropBridge::setDragCancellationState(bool inProgress, bool complete)
{
  std::scoped_lock lock(m_mutex);
  m_dragCancellationInProgress = inProgress;
  m_dragCancellationComplete = complete;
}

void FileDropBridge::onPrimaryLeftMouseDown()
{
  {
    std::scoped_lock lock(m_mutex);
    m_primaryLeftButtonDown = true;
    m_pendingFiles.clear();
    m_canCaptureDrag = true;
    m_dragCancellationInProgress = false;
    m_dragCancellationComplete = false;
    m_lastCapturedSide = 0;
  }
  if (auto *exec = executor()) {
    DropAgentCommand cmd;
    cmd.type = DropAgentCommandType::PrimaryLeftMouseDown;
    exec->execute(cmd);
  }
}

void FileDropBridge::onLeftMouseUp(bool onSecondary)
{
  bool releaseSourceButton = false;
  {
    std::scoped_lock lock(m_mutex);
    m_primaryLeftButtonDown = false;
    releaseSourceButton = m_sourceButtonNeedsRelease;
    m_sourceButtonNeedsRelease = false;
    m_pendingFiles.clear();
    m_canCaptureDrag = false;
    m_dragCancellationInProgress = false;
    m_dragCancellationComplete = false;
    m_visibleSentinelEdgeMask = 0;
  }
  if (auto *exec = executor()) {
    DropAgentCommand cmd;
    cmd.type = DropAgentCommandType::LeftMouseUp;
    cmd.flag = onSecondary && releaseSourceButton;
    exec->execute(cmd);
  }
}

bool FileDropBridge::canSwitchFromPrimary()
{
  int sideToCancel = 0;
  bool needCancel = false;
  {
    std::scoped_lock lock(m_mutex);
    if (m_dragCancellationInProgress) {
      return false;
    }
    const bool validPending = !m_pendingFiles.empty() && ((nowMs() - m_pendingTimestampMs) <= 4000);
    if (validPending && !m_dragCancellationComplete) {
      m_dragCancellationInProgress = true;
      sideToCancel = m_lastCapturedSide;
      needCancel = true;
    }
  }
  if (needCancel) {
    if (auto *exec = executor()) {
      DropAgentCommand cmd;
      cmd.type = DropAgentCommandType::CancelNativeDrag;
      cmd.sideIndex = sideToCancel;
      exec->execute(cmd);
    }
    return false;
  }
  return true;
}

bool FileDropBridge::setPendingDragFiles(std::vector<std::string> files)
{
  std::scoped_lock lock(m_mutex);
  if (!m_canCaptureDrag || files.empty()) {
    LOG_DEBUG("FileDropBridge: ignoring repeated native drag capture without a new primary button press");
    return false;
  }
  m_canCaptureDrag = false;
  m_sourceButtonNeedsRelease = true;
  m_pendingFiles = std::move(files);
  m_pendingTimestampMs = nowMs();
  return true;
}

bool FileDropBridge::hasPendingDragFiles() const
{
  std::scoped_lock lock(m_mutex);
  if (m_pendingFiles.empty()) {
    return false;
  }
  return (nowMs() - m_pendingTimestampMs) <= 4000;
}

bool FileDropBridge::hasActiveDragFiles() const
{
  std::scoped_lock lock(m_mutex);
  return m_activeDrag.active;
}

bool FileDropBridge::hasActiveTransfers() const
{
  if (auto *exec = executor()) {
    return exec->hasActiveTransfers();
  }
  return false;
}

void FileDropBridge::onPendingDragCaptured(std::vector<std::string> files, int sideIndex, int32_t x, int32_t y)
{
  ClientDragMessageSender clientSender;
  bool inClientMode = false;
  {
    std::scoped_lock lock(m_mutex);
    if (m_clientDragSender) {
      inClientMode = true;
      if (!m_clientCanCaptureDrag || m_clientHandoffCommitted || files.empty() || !hasTransferableFiles(files)) {
        return;
      }
      m_clientCanCaptureDrag = false;
      m_clientPendingFiles = files;
      m_clientPendingTimestampMs = nowMs();
      m_clientPendingSide = sideIndex;
      clientSender = m_clientDragSender;
    }
  }

  if (inClientMode) {
    const int clampedSide = std::clamp(sideIndex, 0, 3);
    const std::string msg = serializeClientPreCaptureMessage(static_cast<DragEdgeSide>(clampedSide), x, y);
    LOG_INFO("FileDropBridge: client captured %zu dragged file(s) near edge side=%d", files.size(), clampedSide);
    clientSender(static_cast<uint32_t>(files.size()), msg);
    return;
  }

  if (!setPendingDragFiles(std::move(files))) {
    return;
  }
  {
    std::scoped_lock lock(m_mutex);
    m_lastCapturedSide = sideIndex;
  }
  if (auto *exec = executor()) {
    DropAgentCommand cmd;
    cmd.type = DropAgentCommandType::NudgeAcrossEdge;
    cmd.sideIndex = sideIndex;
    exec->execute(cmd);
  }
}

void FileDropBridge::onDragCancellationCompleted(bool stillPressed)
{
  bool clientHandoffCompleted = false;
  {
    std::scoped_lock lock(m_mutex);
    if (m_clientHandoffCommitted) {
      m_clientCancellationInProgress = false;
      clientHandoffCompleted = true;
    }
  }
  if (clientHandoffCompleted) {
    LOG_INFO("FileDropBridge: client native drag cancellation completed for handoff");
    if (auto *exec = executor()) {
      DropAgentCommand hideCmd;
      hideCmd.type = DropAgentCommandType::HideSentinels;
      exec->execute(hideCmd);

      DropAgentCommand releaseCmd;
      releaseCmd.type = DropAgentCommandType::LeftMouseUp;
      releaseCmd.flag = true;
      exec->execute(releaseCmd);
    }
    return;
  }

  bool hasPending = false;
  {
    std::scoped_lock lock(m_mutex);
    m_dragCancellationInProgress = false;
    m_dragCancellationComplete = true;
    hasPending = !m_pendingFiles.empty() && ((nowMs() - m_pendingTimestampMs) <= 4000);
  }
  if (!hasPending) {
    return;
  }
  if (!stillPressed) {
    cancelActiveDrag();
    return;
  }
  LOG_INFO("FileDropBridge: native drag cancellation completed before screen handoff");
}

void FileDropBridge::onPendingDragCleared()
{
  ClientDragMessageSender senderToNotify;
  bool inClientMode = false;
  {
    std::scoped_lock lock(m_mutex);
    if (m_clientHandoffCommitted || m_clientCancellationInProgress) {
      return;
    }
    if (m_clientDragSender || !m_clientPendingFiles.empty()) {
      inClientMode = true;
      const bool hadPending = !m_clientPendingFiles.empty();
      m_clientPendingFiles.clear();
      m_clientPendingTimestampMs = 0;
      m_clientCanCaptureDrag = false;
      if (hadPending) {
        senderToNotify = m_clientDragSender;
      }
    }
  }
  if (inClientMode) {
    if (senderToNotify) {
      senderToNotify(0, std::string(kMsgDragClientPreCaptureClear));
    }
    if (auto *exec = executor()) {
      DropAgentCommand cmd;
      cmd.type = DropAgentCommandType::LeftMouseUp;
      cmd.flag = false;
      exec->execute(cmd);
    }
    return;
  }
  clearPendingDragOnMouseLeaveOrRelease();
}

void FileDropBridge::onActiveTransfersChanged(bool)
{
}

void FileDropBridge::onExecutorLog(int level, const std::string &message)
{
  (void)level;
  LOG_INFO("FileDropBridge [agent]: %s", message.c_str());
}

std::string FileDropBridge::buildDragStartPayload(const std::string &token, const std::vector<std::string> &files)
{
  std::string msg = serializeDragStartMessage(token);
  for (const auto &f : files) {
    msg += "\n";
    msg += QFileInfo(QString::fromStdString(f)).fileName().toStdString();
  }
  return msg;
}

void FileDropBridge::activateTargetScreen(
    IDropExecutor *exec, BaseClientProxy *target, BaseClientProxy *origin, uint32_t fileCount,
    const std::vector<std::string> &primaryFiles, const std::string &token, int32_t entryX, int32_t entryY,
    bool sendHoverEnter
) const
{
  if (target == nullptr) {
    if (!exec) {
      return;
    }
    DropAgentCommand activeCmd;
    activeCmd.type = DropAgentCommandType::SetDragActive;
    activeCmd.dragActive = true;
    exec->execute(activeCmd);

    if (!token.empty()) {
      DropAgentCommand authCmd;
      authCmd.type = DropAgentCommandType::ReceiverAuthCommand;
      authCmd.text = "GRANT " + token;
      exec->execute(authCmd);
    }

    if (sendHoverEnter) {
      DropAgentCommand hoverCmd;
      hoverCmd.type = DropAgentCommandType::ShowHover;
      hoverCmd.fileCount = fileCount;
      hoverCmd.x = entryX;
      hoverCmd.y = entryY;
      exec->execute(hoverCmd);
    }
    return;
  }

  const std::string startMsg =
      (origin == nullptr) ? buildDragStartPayload(token, primaryFiles) : serializeDragStartMessage(token);
  LOG_INFO(
      "FileDropBridge: entering screen \"%s\" with %u dragged file(s)", target->getName().c_str(), fileCount
  );
  target->sendDragInfo(fileCount, startMsg.data(), startMsg.size());

  if (sendHoverEnter) {
    const std::string hoverMsg = serializeHoverEnterMessage(entryX, entryY);
    target->sendDragInfo(fileCount, hoverMsg.data(), hoverMsg.size());
  }
}

void FileDropBridge::cancelTargetEndpoint(
    IDropExecutor *exec, BaseClientProxy *target, const std::string &token, bool cancelPreparing
) const
{
  if (target == nullptr) {
    if (!exec) {
      return;
    }
    DropAgentCommand hideCmd;
    hideCmd.type = DropAgentCommandType::HideHover;
    exec->execute(hideCmd);

    if (cancelPreparing) {
      DropAgentCommand cancelPrepCmd;
      cancelPrepCmd.type = DropAgentCommandType::CancelPreparing;
      exec->execute(cancelPrepCmd);
    }

    DropAgentCommand activeCmd;
    activeCmd.type = DropAgentCommandType::SetDragActive;
    activeCmd.dragActive = false;
    exec->execute(activeCmd);

    if (!token.empty()) {
      DropAgentCommand tokCmd;
      tokCmd.type = DropAgentCommandType::CancelDropToken;
      tokCmd.text = computeTokenTag(token);
      tokCmd.authToken = token;
      exec->execute(tokCmd);
    }
    return;
  }

  const std::string cancelMsg = serializeDragCancelMessage(token);
  target->sendDragInfo(0, cancelMsg.data(), cancelMsg.size());
}

bool FileDropBridge::canUnlockEdgeSwitchForDrag(BaseClientProxy *activeScreen) const
{
  std::scoped_lock lock(m_mutex);
  if (m_activeDrag.active) {
    return true;
  }
  const uint64_t now = nowMs();
  if (activeScreen == nullptr) {
    return !m_pendingFiles.empty() && ((now - m_pendingTimestampMs) <= 4000);
  }
  return (m_preCapturedClient == activeScreen) && (m_preCapturedClientFileCount > 0) &&
         ((now - m_preCapturedClientTimestampMs) <= 4000);
}

bool FileDropBridge::isDragActiveForSwitch(BaseClientProxy *fromScreen, BaseClientProxy *toScreen) const
{
  if (fromScreen == toScreen) {
    return false;
  }
  std::scoped_lock lock(m_mutex);
  if (m_activeDrag.active) {
    return true;
  }
  const uint64_t now = nowMs();
  if (fromScreen == nullptr) {
    return !m_pendingFiles.empty() && ((now - m_pendingTimestampMs) <= 4000);
  }
  return (m_preCapturedClient == fromScreen) && (m_preCapturedClientFileCount > 0) &&
         ((now - m_preCapturedClientTimestampMs) <= 4000);
}

void FileDropBridge::prepareScreenSwitch(BaseClientProxy *fromScreen, BaseClientProxy *toScreen)
{
  if (fromScreen == toScreen) {
    return;
  }

  if (fromScreen == nullptr) {
    if (auto *exec = executor()) {
      DropAgentCommand hideCmd;
      hideCmd.type = DropAgentCommandType::HideSentinels;
      exec->execute(hideCmd);
    }
    return;
  }

  BaseClientProxy *clientToCommit = nullptr;
  {
    std::scoped_lock lock(m_mutex);
    if (!m_activeDrag.active && m_preCapturedClient == fromScreen && m_preCapturedClientFileCount > 0 &&
        ((nowMs() - m_preCapturedClientTimestampMs) <= 4000)) {
      clientToCommit = fromScreen;
    }
  }
  if (clientToCommit) {
    LOG_INFO("FileDropBridge: committing drag handoff from \"%s\"", clientToCommit->getName().c_str());
    clientToCommit->sendDragInfo(0, kMsgDragCommitHandoff.data(), kMsgDragCommitHandoff.size());
  }
}

void FileDropBridge::onScreenSwitch(BaseClientProxy *fromScreen, BaseClientProxy *toScreen, int32_t entryX, int32_t entryY)
{
  performScreenSwitch(fromScreen, toScreen, entryX, entryY, true);
}

void FileDropBridge::performScreenSwitch(
    BaseClientProxy *fromScreen, BaseClientProxy *toScreen, int32_t entryX, int32_t entryY, bool sendHoverEnter
)
{
  if (fromScreen == toScreen) {
    return;
  }

  if (fromScreen == nullptr) {
    bool shouldHideSentinels = false;
    {
      std::scoped_lock lock(m_mutex);
      shouldHideSentinels = (m_visibleSentinelEdgeMask != 0);
      m_visibleSentinelEdgeMask = 0;
    }
    if (shouldHideSentinels) {
      if (auto *exec = executor()) {
        DropAgentCommand hideCmd;
        hideCmd.type = DropAgentCommandType::HideSentinels;
        exec->execute(hideCmd);
      }
    }
  }

  IDropExecutor *exec = executor();
  ActiveCrossDrag newDrag;
  bool startNewDrag = false;
  bool returningToOrigin = false;
  bool hoppingTarget = false;
  BaseClientProxy *oldTargetToCancel = nullptr;
  std::string oldTokenToCancel;
  BaseClientProxy *originToCancel = nullptr;

  {
    std::scoped_lock lock(m_mutex);
    const uint64_t now = nowMs();

    if (!m_activeDrag.active) {
      if (fromScreen == nullptr) {
        if (m_pendingFiles.empty() || (now - m_pendingTimestampMs) > 4000) {
          m_pendingFiles.clear();
          return;
        }
        std::vector<std::string> files = std::move(m_pendingFiles);
        m_pendingFiles.clear();
        if (!hasTransferableFiles(files)) {
          LOG_INFO("FileDropBridge: dragged path(s) contain no transferable files; skipping cross-screen drag");
          return;
        }
        m_activeDrag.active = true;
        m_activeDrag.originClient = nullptr;
        m_activeDrag.targetClient = toScreen;
        m_activeDrag.fileCount = static_cast<uint32_t>(files.size());
        m_activeDrag.primaryFiles = std::move(files);
        m_activeDrag.activeToken = generateAuthToken();
        newDrag = m_activeDrag;
        startNewDrag = true;
      } else if (m_preCapturedClient == fromScreen && m_preCapturedClientFileCount > 0 &&
                 (now - m_preCapturedClientTimestampMs) <= 4000) {
        const uint32_t count = m_preCapturedClientFileCount;
        m_preCapturedClient = nullptr;
        m_preCapturedClientFileCount = 0;
        m_preCapturedClientTimestampMs = 0;

        m_activeDrag.active = true;
        m_activeDrag.originClient = fromScreen;
        m_activeDrag.targetClient = toScreen;
        m_activeDrag.fileCount = count;
        m_activeDrag.primaryFiles.clear();
        m_activeDrag.activeToken = generateAuthToken();
        newDrag = m_activeDrag;
        startNewDrag = true;
      } else {
        if (m_preCapturedClient == fromScreen) {
          m_preCapturedClient = nullptr;
          m_preCapturedClientFileCount = 0;
          m_preCapturedClientTimestampMs = 0;
        }
        return;
      }
    } else {
      if (toScreen == m_activeDrag.targetClient) {
        return;
      }
      oldTargetToCancel = m_activeDrag.targetClient;
      oldTokenToCancel = m_activeDrag.activeToken;

      if (toScreen == m_activeDrag.originClient) {
        originToCancel = m_activeDrag.originClient;
        m_activeDrag = {};
        m_pendingFiles.clear();
        m_canCaptureDrag = false;
        returningToOrigin = true;
      } else {
        m_activeDrag.targetClient = toScreen;
        m_activeDrag.activeToken = generateAuthToken();
        newDrag = m_activeDrag;
        hoppingTarget = true;
      }
    }
  }

  if (startNewDrag) {
    activateTargetScreen(
        exec, newDrag.targetClient, newDrag.originClient, newDrag.fileCount, newDrag.primaryFiles, newDrag.activeToken,
        entryX, entryY, sendHoverEnter
    );
    return;
  }

  if (returningToOrigin) {
    LOG_INFO("FileDropBridge: cursor returned to origin screen before release; cancelling drag");
    cancelTargetEndpoint(exec, oldTargetToCancel, oldTokenToCancel, false);
    if (originToCancel) {
      sendOriginCancelMessage(originToCancel, oldTokenToCancel);
    }
    return;
  }

  if (hoppingTarget) {
    cancelTargetEndpoint(exec, oldTargetToCancel, oldTokenToCancel, false);
    activateTargetScreen(
        exec, newDrag.targetClient, newDrag.originClient, newDrag.fileCount, newDrag.primaryFiles, newDrag.activeToken,
        entryX, entryY, sendHoverEnter
    );
  }
}

void FileDropBridge::onServerReceiveClientDragInfo(BaseClientProxy *client, uint16_t fileCount, const std::string &info)
{
  if (!client) {
    return;
  }
  std::string requestId;
  std::string targetPeer;
  if (parseDirectSendRequestMessage(info, requestId, targetPeer)) {
    const auto reply = [client, fileCount](const std::string &message) {
      client->sendDragInfo(fileCount, message.data(), message.size());
    };
    if (fileCount == 0) {
      reply(serializeDirectSendCancelMessage(requestId));
      return;
    }
    const auto token = generateAuthToken();
    const auto tag = computeTokenTag(token);
    if (targetPeer.empty() || targetPeer == kServerPeerTargetMarker) {
      auto *exec = executor();
      if (!exec) {
        reply(serializeDirectSendCancelMessage(requestId));
        return;
      }
      DropAgentCommand active;
      active.type = DropAgentCommandType::SetDragActive;
      active.dragActive = true;
      exec->execute(active);
      DropAgentCommand auth;
      auth.type = DropAgentCommandType::ReceiverAuthCommand;
      auth.text = "GRANT " + token;
      exec->execute(auth);
      DropAgentCommand preparing;
      preparing.type = DropAgentCommandType::BeginPreparing;
      preparing.fileCount = fileCount;
      exec->execute(preparing);
      DropAgentCommand drop;
      drop.type = DropAgentCommandType::DropTarget;
      drop.fileCount = fileCount;
      drop.x = kDirectSendDropCoord;
      drop.y = kDirectSendDropCoord;
      drop.text = tag;
      exec->execute(drop);
      reply(serializeDirectSendGrantMessage(requestId, token, kServerPeerTargetMarker));
      return;
    }

    BaseClientProxy *destination = nullptr;
    {
      std::scoped_lock lock(m_mutex);
      for (const auto &[name, proxy] : m_serverClients) {
        if (name == targetPeer && proxy != client) {
          destination = proxy;
          break;
        }
      }
    }
    const auto peerIp = destination ? getClientPeerIp(destination) : std::string{};
    const auto grant = serializeDirectSendGrantMessage(requestId, token, peerIp);
    if (grant.empty()) {
      reply(serializeDirectSendCancelMessage(requestId));
      return;
    }
    const auto start = serializeDragStartMessage(token);
    const auto drop = serializeDragDropMessage(kDirectSendDropCoord, kDirectSendDropCoord, tag);
    destination->sendDragInfo(fileCount, start.data(), start.size());
    destination->sendDragInfo(fileCount, drop.data(), drop.size());
    reply(grant);
    return;
  }

  DragEdgeSide side = DragEdgeSide::Left;
  int32_t x = 0;
  int32_t y = 0;
  if (parseClientPreCaptureMessage(info, side, x, y)) {
    std::scoped_lock lock(m_mutex);
    if (m_activeDrag.active || fileCount == 0) {
      return;
    }
    m_preCapturedClient = client;
    m_preCapturedClientFileCount = fileCount;
    m_preCapturedClientSide = side;
    m_preCapturedClientTimestampMs = nowMs();
    LOG_INFO(
        "FileDropBridge: recorded client \"%s\" pre-capture (%u file(s))", client->getName().c_str(), fileCount
    );
    return;
  }

  if (isClientPreCaptureClearMessage(info)) {
    std::scoped_lock lock(m_mutex);
    if (m_preCapturedClient == client) {
      m_preCapturedClient = nullptr;
      m_preCapturedClientFileCount = 0;
      m_preCapturedClientTimestampMs = 0;
    }
  }
}

void FileDropBridge::onLeavePrimary(BaseClientProxy *dstClient)
{
  performScreenSwitch(nullptr, dstClient, 0, 0, false);
}

void FileDropBridge::onReturnToPrimary()
{
  BaseClientProxy *oldTarget = nullptr;
  {
    std::scoped_lock lock(m_mutex);
    if (!m_activeDrag.active || m_activeDrag.originClient != nullptr) {
      return;
    }
    oldTarget = m_activeDrag.targetClient;
  }
  performScreenSwitch(oldTarget, nullptr, 0, 0, false);
}

void FileDropBridge::onClientDisconnected(BaseClientProxy *client)
{
  if (!client) {
    return;
  }
  ActiveCrossDrag disconnectedDrag;
  bool hadActiveDrag = false;
  std::optional<std::vector<std::string>> targets;
  {
    std::scoped_lock lock(m_mutex);
    if (std::erase_if(m_serverClients, [client](const auto &entry) { return entry.second == client; }) != 0) {
      targets = serverTransferTargets(m_serverClients);
    }
    if (m_preCapturedClient == client) {
      m_preCapturedClient = nullptr;
      m_preCapturedClientFileCount = 0;
      m_preCapturedClientTimestampMs = 0;
    }
    if (m_activeDrag.active && (m_activeDrag.originClient == client || m_activeDrag.targetClient == client)) {
      disconnectedDrag = std::move(m_activeDrag);
      m_activeDrag = {};
      m_pendingFiles.clear();
      m_canCaptureDrag = false;
      m_sourceButtonNeedsRelease = false;
      hadActiveDrag = true;
    }
  }

  if (targets) {
    notifyTransferTargetsChanged(*targets);
  }
  if (!hadActiveDrag) {
    return;
  }

  IDropExecutor *exec = executor();
  if (disconnectedDrag.originClient == client) {
    LOG_INFO("FileDropBridge: drag origin client disconnected; cancelling target state");
    cancelTargetEndpoint(exec, disconnectedDrag.targetClient, disconnectedDrag.activeToken, true);
  } else {
    LOG_INFO("FileDropBridge: active drag target client disconnected; clearing target pointer");
    if (disconnectedDrag.originClient != nullptr) {
      sendOriginCancelMessage(disconnectedDrag.originClient, disconnectedDrag.activeToken);
    }
  }
}

void FileDropBridge::cancelActiveDrag()
{
  ActiveCrossDrag cancelledDrag;
  {
    std::scoped_lock lock(m_mutex);
    LOG_INFO("FileDropBridge: Escape pressed; cancelling file drag");
    m_pendingFiles.clear();
    m_canCaptureDrag = false;
    m_preCapturedClient = nullptr;
    m_preCapturedClientFileCount = 0;
    m_preCapturedClientTimestampMs = 0;
    if (m_activeDrag.active) {
      cancelledDrag = std::move(m_activeDrag);
    }
    m_activeDrag = {};
  }

  if (cancelledDrag.active) {
    cancelTargetEndpoint(executor(), cancelledDrag.targetClient, cancelledDrag.activeToken, true);
    if (cancelledDrag.originClient != nullptr) {
      sendOriginCancelMessage(cancelledDrag.originClient, cancelledDrag.activeToken);
    }
  }
}

void FileDropBridge::onServerLeftMouseUp(BaseClientProxy *activeScreen, int32_t x, int32_t y)
{
  bool releaseSourceButton = false;
  ActiveCrossDrag dropDrag;
  bool executeDrop = false;
  {
    std::scoped_lock lock(m_mutex);
    releaseSourceButton = m_sourceButtonNeedsRelease;
    m_sourceButtonNeedsRelease = false;
    m_pendingFiles.clear();
    m_canCaptureDrag = false;
    m_dragCancellationInProgress = false;
    m_dragCancellationComplete = false;
    m_visibleSentinelEdgeMask = 0;
    if (activeScreen != nullptr && m_preCapturedClient == activeScreen) {
      m_preCapturedClient = nullptr;
      m_preCapturedClientFileCount = 0;
      m_preCapturedClientTimestampMs = 0;
    }
    if (m_activeDrag.active) {
      dropDrag = std::move(m_activeDrag);
      m_activeDrag = {};
      executeDrop = true;
    }
  }

  IDropExecutor *exec = executor();
  if (exec && (activeScreen != nullptr || !executeDrop || releaseSourceButton)) {
    DropAgentCommand cmd;
    cmd.type = DropAgentCommandType::LeftMouseUp;
    cmd.flag = (activeScreen != nullptr) && releaseSourceButton;
    exec->execute(cmd);
  }

  if (!executeDrop) {
    return;
  }

  if (dropDrag.targetClient != activeScreen) {
    LOG_WARN("FileDropBridge: mouse up screen mismatch; cancelling mismatched active drag");
    cancelTargetEndpoint(exec, dropDrag.targetClient, dropDrag.activeToken, true);
    if (dropDrag.originClient) {
      sendOriginCancelMessage(dropDrag.originClient, dropDrag.activeToken);
    }
    return;
  }

  // Case 1: Client A -> Primary drop
  if (dropDrag.targetClient == nullptr) {
    if (!dropDrag.originClient) {
      return;
    }
    const std::string tag = computeTokenTag(dropDrag.activeToken);
    if (exec) {
      DropAgentCommand hideCmd;
      hideCmd.type = DropAgentCommandType::HideHover;
      exec->execute(hideCmd);

      DropAgentCommand prepCmd;
      prepCmd.type = DropAgentCommandType::BeginPreparing;
      prepCmd.fileCount = dropDrag.fileCount;
      exec->execute(prepCmd);

      DropAgentCommand targetCmd;
      targetCmd.type = DropAgentCommandType::DropTarget;
      targetCmd.fileCount = dropDrag.fileCount;
      targetCmd.x = x;
      targetCmd.y = y;
      targetCmd.text = tag;
      exec->execute(targetCmd);
    }
    const std::string sendDropMsg =
        serializeSendDropMessage(x, y, dropDrag.activeToken, kServerPeerTargetMarker);
    LOG_INFO(
        "FileDropBridge: drop at (%d,%d) on primary from \"%s\" (%u file(s))", x, y,
        dropDrag.originClient->getName().c_str(), dropDrag.fileCount
    );
    dropDrag.originClient->sendDragInfo(dropDrag.fileCount, sendDropMsg.data(), sendDropMsg.size());
    return;
  }

  // Case 2 & 3: Drop on remote client B (from Primary OR from Client A)
  const std::string peerIp = getClientPeerIp(dropDrag.targetClient);
  const bool validOriginPayload =
      (dropDrag.originClient != nullptr) || hasTransferableFiles(dropDrag.primaryFiles);
  if (peerIp.empty() || !validOriginPayload) {
    LOG_WARN(
        "FileDropBridge: cannot complete drop onto \"%s\" (invalid peer IP or payload)",
        dropDrag.targetClient->getName().c_str()
    );
    cancelTargetEndpoint(exec, dropDrag.targetClient, dropDrag.activeToken, true);
    if (dropDrag.originClient != nullptr) {
      sendOriginCancelMessage(dropDrag.originClient, dropDrag.activeToken);
    }
    return;
  }

  const std::string tag = computeTokenTag(dropDrag.activeToken);
  const std::string dropMsg = serializeDragDropMessage(x, y, tag);
  dropDrag.targetClient->sendDragInfo(dropDrag.fileCount, dropMsg.data(), dropMsg.size());

  if (dropDrag.originClient == nullptr) {
    LOG_INFO(
        "FileDropBridge: drop at (%d,%d) on \"%s\" (%s), sending %zu file(s) via LocalSend", x, y,
        dropDrag.targetClient->getName().c_str(), peerIp.c_str(), dropDrag.primaryFiles.size()
    );
    if (exec) {
      DropAgentCommand cmd;
      cmd.type = DropAgentCommandType::SendFiles;
      cmd.text = peerIp;
      cmd.items = std::move(dropDrag.primaryFiles);
      cmd.authToken = std::move(dropDrag.activeToken);
      exec->execute(cmd);
    }
  } else {
    LOG_INFO(
        "FileDropBridge: multi-hop drop at (%d,%d) from \"%s\" to \"%s\" (%s, %u file(s))", x, y,
        dropDrag.originClient->getName().c_str(), dropDrag.targetClient->getName().c_str(), peerIp.c_str(),
        dropDrag.fileCount
    );
    const std::string sendDropMsg = serializeSendDropMessage(x, y, dropDrag.activeToken, peerIp);
    dropDrag.originClient->sendDragInfo(dropDrag.fileCount, sendDropMsg.data(), sendDropMsg.size());
  }
}

void FileDropBridge::onSecondaryLeftMouseUp(BaseClientProxy *activeClient, int32_t x, int32_t y)
{
  if (activeClient == nullptr) {
    return;
  }
  bool hasActiveForClient = false;
  {
    std::scoped_lock lock(m_mutex);
    hasActiveForClient = m_activeDrag.active && (m_activeDrag.targetClient == activeClient);
  }
  if (!hasActiveForClient) {
    return;
  }
  onServerLeftMouseUp(activeClient, x, y);
}

void FileDropBridge::refreshClientReceiverHosts()
{
  std::vector<std::string> hosts;
  {
    std::scoped_lock lock(m_mutex);
    if (!m_clientServerHost.empty()) {
      hosts.push_back(m_clientServerHost);
    }
    for (const auto &peer : m_clientPeerHosts) {
      if (!peer.empty()) {
        hosts.push_back(peer);
      }
    }
  }
  applyReceiverHosts(executor(), hosts);
}

void FileDropBridge::startReceiver(const std::string &serverHost)
{
  startClientReceiver(serverHost);
}

void FileDropBridge::startClientReceiver(const std::string &serverHost)
{
  std::vector<std::string> targets;
  {
    std::scoped_lock lock(m_mutex);
    m_clientServerHost = serverHost;
    if (!serverHost.empty() && m_clientTransferTargets.empty()) {
      m_clientTransferTargets.emplace_back(kServerPeerTargetMarker);
    }
    targets = m_clientTransferTargets;
  }
  notifyTransferTargetsChanged(targets);
  refreshClientReceiverHosts();
}

void FileDropBridge::stopReceiver()
{
  {
    std::scoped_lock lock(m_mutex);
    m_clientDirectPending.clear();
    m_clientTransferTargets.clear();
    m_serverClients.clear();
    m_clientLeftButtonDown = false;
    m_clientServerHost.clear();
    m_clientPeerHosts.clear();
    m_clientEdgeSegments.clear();
    m_serverPeerHosts.clear();
  }
  notifyTransferTargetsChanged({});
  if (auto *exec = executor()) {
    DropAgentCommand cmd;
    cmd.type = DropAgentCommandType::StopReceiver;
    exec->execute(cmd);
  }
}

void FileDropBridge::setClientDragMessageSender(ClientDragMessageSender sender)
{
  std::optional<std::vector<std::string>> targets;
  {
    std::scoped_lock lock(m_mutex);
    m_clientDragSender = std::move(sender);
    if (m_clientDragSender && !m_clientServerHost.empty()) {
      targets = m_clientTransferTargets;
    }
  }
  if (targets) {
    notifyTransferTargetsChanged(*targets);
  }
}

void FileDropBridge::onClientMouseLeftDown()
{
  ClientDragMessageSender senderToNotify;
  {
    std::scoped_lock lock(m_mutex);
    m_clientLeftButtonDown = true;
    const bool hadPending = !m_clientPendingFiles.empty();
    m_clientPendingFiles.clear();
    m_clientPendingTimestampMs = 0;
    m_clientCanCaptureDrag = true;
    m_clientHandoffCommitted = false;
    m_clientCancellationInProgress = false;
    if (hadPending) {
      senderToNotify = m_clientDragSender;
    }
  }
  if (senderToNotify) {
    senderToNotify(0, std::string(kMsgDragClientPreCaptureClear));
  }
  if (auto *exec = executor()) {
    DropAgentCommand cmd;
    cmd.type = DropAgentCommandType::PrimaryLeftMouseDown;
    exec->execute(cmd);
  }
}

void FileDropBridge::onClientMouseLeftUp()
{
  ClientDragMessageSender senderToNotify;
  bool shouldReleaseSentinels = false;
  {
    std::scoped_lock lock(m_mutex);
    m_clientLeftButtonDown = false;
    if (m_clientHandoffCommitted) {
      return;
    }
    const bool hadPending = !m_clientPendingFiles.empty();
    m_clientPendingFiles.clear();
    m_clientPendingTimestampMs = 0;
    m_clientCanCaptureDrag = false;
    m_clientSentinelVisible = false;
    shouldReleaseSentinels = true;
    if (hadPending) {
      senderToNotify = m_clientDragSender;
    }
  }
  if (senderToNotify) {
    senderToNotify(0, std::string(kMsgDragClientPreCaptureClear));
  }
  if (shouldReleaseSentinels) {
    if (auto *exec = executor()) {
      DropAgentCommand cmd;
      cmd.type = DropAgentCommandType::LeftMouseUp;
      cmd.flag = false;
      exec->execute(cmd);
    }
  }
}

void FileDropBridge::onClientMouseMoveWithShape(
    int32_t x, int32_t y, int32_t ax, int32_t ay, int32_t aw, int32_t ah
)
{
  auto *exec = executor();
  if (!exec) {
    return;
  }

  if (exec->isHoverVisible()) {
    DropAgentCommand hoverCmd;
    hoverCmd.type = DropAgentCommandType::UpdateHoverCursor;
    hoverCmd.x = x;
    hoverCmd.y = y;
    exec->execute(hoverCmd);
    return;
  }

#if !defined(_WIN32)
  bool trackedDown = false;
#endif
  std::vector<DragEdgeSegment> segs;
  bool blocked = false;
  bool wasVisible = false;
  {
    std::scoped_lock lock(m_mutex);
#if !defined(_WIN32)
    trackedDown = m_clientLeftButtonDown;
#endif
    segs = m_clientEdgeSegments;
    blocked = m_clientHandoffCommitted || m_clientCancellationInProgress;
    wasVisible = m_clientSentinelVisible;
  }

#if defined(_WIN32)
  const bool leftButtonDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
#else
  const bool sentinelSupported = platform_drop::isNativeEdgeSentinelSupported();
  const bool leftButtonDown = sentinelSupported && platform_drop::isNativeLeftMouseButtonDown(trackedDown);
#endif
  if (segs.empty() || blocked || aw <= 0 || ah <= 0 || !leftButtonDown) {
    if (wasVisible) {
      {
        std::scoped_lock lock(m_mutex);
        m_clientSentinelVisible = false;
      }
      DropAgentCommand hideCmd;
      hideCmd.type = DropAgentCommandType::HideSentinels;
      exec->execute(hideCmd);
    }
    return;
  }

  const uint32_t activeSides = activeSidesMaskFromSegments(segs);
  const uint32_t nearMask = computeNearEdgeMask(x, y, ax, ay, aw, ah, activeSides);
  {
    std::scoped_lock lock(m_mutex);
    m_clientSentinelVisible = (nearMask != 0);
  }
  DropAgentCommand moveCmd;
  moveCmd.type = DropAgentCommandType::PrimaryMouseMove;
  moveCmd.x = x;
  moveCmd.y = y;
  moveCmd.ax = ax;
  moveCmd.ay = ay;
  moveCmd.aw = aw;
  moveCmd.ah = ah;
  moveCmd.activeSides = activeSides;
  moveCmd.edgeSegments = std::move(segs);
  exec->execute(moveCmd);
}

void FileDropBridge::onClientLeave()
{
  bool keepPendingForHandoff = false;
  bool wasSentinelVisible = false;
  {
    std::scoped_lock lock(m_mutex);
    m_clientLeftButtonDown = false;
    keepPendingForHandoff = m_clientHandoffCommitted || m_clientCancellationInProgress;
    if (!keepPendingForHandoff) {
      m_clientPendingFiles.clear();
      m_clientPendingTimestampMs = 0;
      m_clientCanCaptureDrag = false;
    }
    wasSentinelVisible = m_clientSentinelVisible;
    m_clientSentinelVisible = false;
  }
  if (auto *exec = executor()) {
    if (exec->isHoverVisible()) {
      DropAgentCommand hideHoverCmd;
      hideHoverCmd.type = DropAgentCommandType::HideHover;
      exec->execute(hideHoverCmd);
    }
    if (wasSentinelVisible) {
      DropAgentCommand hideCmd;
      hideCmd.type = DropAgentCommandType::HideSentinels;
      exec->execute(hideCmd);
    }
  }
}

void FileDropBridge::onClientMouseMove(int32_t x, int32_t y)
{
  auto *exec = executor();
  if (!exec || !exec->isHoverVisible()) {
    return;
  }
  DropAgentCommand cmd;
  cmd.type = DropAgentCommandType::UpdateHoverCursor;
  cmd.x = x;
  cmd.y = y;
  exec->execute(cmd);
}

bool FileDropBridge::isClientHoverVisible() const
{
  auto *exec = executor();
  return exec ? exec->isHoverVisible() : false;
}

uint32_t FileDropBridge::clientHoverFileCount() const
{
  auto *exec = executor();
  return exec ? exec->hoverFileCount() : 0;
}

QPoint FileDropBridge::clientHoverCursorPos() const
{
  auto *exec = executor();
  return exec ? exec->hoverCursorPos() : QPoint{};
}

TransferStatusOverlay::Phase FileDropBridge::receiverOverlayPhase() const
{
  auto *exec = executor();
  return exec ? exec->receiverOverlayPhase() : TransferStatusOverlay::Phase::Idle;
}

uint32_t FileDropBridge::receiverPreparingFileCount() const
{
  auto *exec = executor();
  return exec ? exec->receiverPreparingFileCount() : 0;
}

TransferStatusOverlay &FileDropBridge::overlayFor(bool receiving)
{
  return executor()->overlayFor(receiving);
}

void FileDropBridge::onClientReceiveDragInfo(uint16_t fileCount, const std::string &info)
{
  std::vector<std::string> targets;
  if (parseTransferTargetsMessage(info, targets)) {
    {
      std::scoped_lock lock(m_mutex);
      m_clientTransferTargets = targets;
    }
    notifyTransferTargetsChanged(targets);
    return;
  }
  std::string requestId;
  std::string token;
  std::string targetIp;
  if (parseDirectSendGrantMessage(info, requestId, token, targetIp)) {
    DropAgentCommand cmd;
    {
      std::scoped_lock lock(m_mutex);
      const auto pending = std::find_if(m_clientDirectPending.begin(), m_clientDirectPending.end(), [&](const auto &entry) {
        return entry.requestId == requestId;
      });
      if (pending == m_clientDirectPending.end()) {
        return;
      }
      cmd.items = std::move(pending->files);
      m_clientDirectPending.erase(pending);
      cmd.text = targetIp == kServerPeerTargetMarker ? m_clientServerHost : targetIp;
    }
    if (auto *exec = executor(); exec && !cmd.text.empty() && hasTransferableFiles(cmd.items)) {
      cmd.type = DropAgentCommandType::SendFiles;
      cmd.authToken = std::move(token);
      exec->execute(cmd);
    }
    return;
  }
  if (parseDirectSendCancelMessage(info, requestId)) {
    std::scoped_lock lock(m_mutex);
    std::erase_if(m_clientDirectPending, [&](const auto &entry) { return entry.requestId == requestId; });
    return;
  }

  auto *exec = executor();
  if (!exec) {
    return;
  }

  std::vector<DragEdgeSegment> edges;
  if (parseDragEdgesMessage(info, edges)) {
    std::scoped_lock lock(m_mutex);
    m_clientEdgeSegments = std::move(edges);
    return;
  }

  std::vector<std::string> peers;
  if (parseDragPeersMessage(info, peers)) {
    {
      std::scoped_lock lock(m_mutex);
      m_clientPeerHosts = std::move(peers);
    }
    refreshClientReceiverHosts();
    return;
  }

  if (isClientPreCaptureClearMessage(info)) {
    return;
  }

  if (isCommitHandoffMessage(info)) {
    bool hasPending = false;
    int sideIndex = 0;
    {
      std::scoped_lock lock(m_mutex);
      m_clientHandoffCommitted = true;
      m_clientCanCaptureDrag = false;
      m_clientSentinelVisible = false;
      hasPending = !m_clientPendingFiles.empty();
      sideIndex = m_clientPendingSide;
      if (hasPending) {
        m_clientCancellationInProgress = true;
      }
    }
    if (hasPending) {
      DropAgentCommand cancelCmd;
      cancelCmd.type = DropAgentCommandType::CancelNativeDrag;
      cancelCmd.sideIndex = sideIndex;
      exec->execute(cancelCmd);
    } else {
      DropAgentCommand hideCmd;
      hideCmd.type = DropAgentCommandType::HideSentinels;
      exec->execute(hideCmd);

      DropAgentCommand releaseCmd;
      releaseCmd.type = DropAgentCommandType::LeftMouseUp;
      releaseCmd.flag = true;
      exec->execute(releaseCmd);
    }
    return;
  }

  int32_t dropX = 0;
  int32_t dropY = 0;
  std::string sendToken;
  std::string targetIp;
  if (parseSendDropMessage(info, dropX, dropY, sendToken, targetIp)) {
    std::vector<std::string> files;
    std::string serverHost;
    {
      std::scoped_lock lock(m_mutex);
      files = std::move(m_clientPendingFiles);
      m_clientPendingFiles.clear();
      m_clientPendingTimestampMs = 0;
      m_clientHandoffCommitted = false;
      m_clientCancellationInProgress = false;
      m_clientCanCaptureDrag = false;
      serverHost = m_clientServerHost;
    }
    const std::string resolvedTarget = (targetIp == kServerPeerTargetMarker) ? serverHost : targetIp;
    if (files.empty() || !hasTransferableFiles(files) || resolvedTarget.empty()) {
      LOG_WARN("FileDropBridge: ignoring DESKSEND_SEND_DROP with missing files or target host");
      return;
    }
    LOG_INFO(
        "FileDropBridge: executing client send of %zu file(s) to %s (drop %d,%d)", files.size(),
        resolvedTarget.c_str(), dropX, dropY
    );
    DropAgentCommand sendCmd;
    sendCmd.type = DropAgentCommandType::SendFiles;
    sendCmd.text = resolvedTarget;
    sendCmd.items = std::move(files);
    sendCmd.authToken = std::move(sendToken);
    exec->execute(sendCmd);
    return;
  }

  std::string sendCancelToken;
  if (parseSendCancelMessage(info, sendCancelToken)) {
    {
      std::scoped_lock lock(m_mutex);
      m_clientPendingFiles.clear();
      m_clientPendingTimestampMs = 0;
      m_clientHandoffCommitted = false;
      m_clientCancellationInProgress = false;
      m_clientCanCaptureDrag = false;
      m_clientSentinelVisible = false;
    }
    DropAgentCommand hideCmd;
    hideCmd.type = DropAgentCommandType::HideSentinels;
    exec->execute(hideCmd);

    DropAgentCommand resetCmd;
    resetCmd.type = DropAgentCommandType::LeftMouseUp;
    resetCmd.flag = false;
    exec->execute(resetCmd);
    return;
  }

  int32_t hx = 0;
  int32_t hy = 0;
  if (parseHoverEnterMessage(info, hx, hy)) {
    DropAgentCommand cmd;
    cmd.type = DropAgentCommandType::ShowHover;
    cmd.fileCount = fileCount;
    cmd.x = hx;
    cmd.y = hy;
    exec->execute(cmd);
    return;
  }
  if (isHoverLeaveMessage(info)) {
    DropAgentCommand cmd;
    cmd.type = DropAgentCommandType::HideHover;
    exec->execute(cmd);
    return;
  }

  const std::string dropPrefix = "DESKSEND_DROP:";
  if (info.rfind(dropPrefix, 0) == 0) {
    DropAgentCommand hideCmd;
    hideCmd.type = DropAgentCommandType::HideHover;
    exec->execute(hideCmd);

    DropAgentCommand prepCmd;
    prepCmd.type = DropAgentCommandType::BeginPreparing;
    prepCmd.fileCount = fileCount;
    exec->execute(prepCmd);

    const std::string coords = info.substr(dropPrefix.size());
    auto comma = coords.find(',');
    if (comma != std::string::npos) {
      const int32_t x = std::atoi(coords.substr(0, comma).c_str());
      const std::string rest = coords.substr(comma + 1);
      const auto tagSep = rest.find(':');
      const int32_t y = std::atoi((tagSep == std::string::npos ? rest : rest.substr(0, tagSep)).c_str());
      std::string tokenTag = (tagSep == std::string::npos) ? std::string{} : rest.substr(tagSep + 1);
      const size_t nl = tokenTag.find_first_of("\r\n");
      if (nl != std::string::npos) {
        tokenTag.resize(nl);
      }

      DropAgentCommand dropCmd;
      dropCmd.type = DropAgentCommandType::DropTarget;
      dropCmd.fileCount = fileCount;
      dropCmd.x = x;
      dropCmd.y = y;
      dropCmd.text = std::move(tokenTag);
      exec->execute(dropCmd);
    }
  } else if (info.rfind("DESKSEND_START", 0) == 0) {
    DropAgentCommand activeCmd;
    activeCmd.type = DropAgentCommandType::SetDragActive;
    activeCmd.dragActive = true;
    exec->execute(activeCmd);

    std::string token;
    if (parseDragStartMessage(info, token)) {
      DropAgentCommand authCmd;
      authCmd.type = DropAgentCommandType::ReceiverAuthCommand;
      authCmd.text = "GRANT " + token;
      exec->execute(authCmd);
    }
    LOG_INFO("FileDropBridge: incoming drag started (%u file(s))", fileCount);
  } else if (info.rfind("DESKSEND_CANCEL", 0) == 0) {
    DropAgentCommand hideCmd;
    hideCmd.type = DropAgentCommandType::HideHover;
    exec->execute(hideCmd);

    DropAgentCommand cancelPrepCmd;
    cancelPrepCmd.type = DropAgentCommandType::CancelPreparing;
    exec->execute(cancelPrepCmd);

    DropAgentCommand activeCmd;
    activeCmd.type = DropAgentCommandType::SetDragActive;
    activeCmd.dragActive = false;
    exec->execute(activeCmd);

    std::string token;
    const std::string cancelPrefix = "DESKSEND_CANCEL:";
    if (info.rfind(cancelPrefix, 0) == 0) {
      token = info.substr(cancelPrefix.size());
      const size_t endPos = token.find_first_of("\r\n");
      if (endPos != std::string::npos) {
        token.resize(endPos);
      }
    }
    if (!token.empty() && token.find_first_of(" \t\r\n") == std::string::npos) {
      DropAgentCommand cancelTokCmd;
      cancelTokCmd.type = DropAgentCommandType::CancelDropToken;
      cancelTokCmd.text = computeTokenTag(token);
      cancelTokCmd.authToken = token;
      exec->execute(cancelTokCmd);
    }
    LOG_INFO("FileDropBridge: incoming drag cancelled");
  }
}

void FileDropBridge::handleReceiveSessionStarted(size_t expectedFiles, const std::string &tokenTag)
{
  if (auto *ip = inProcessExecutorForTesting()) {
    ip->handleReceiveSessionStarted(expectedFiles, tokenTag);
  }
}

void FileDropBridge::handleReceiveSessionFinished(bool cancelled)
{
  if (auto *ip = inProcessExecutorForTesting()) {
    ip->handleReceiveSessionFinished(cancelled);
  }
}

std::string FileDropBridge::getClientPeerIp(BaseClientProxy *client)
{
  if (!client) {
    return {};
  }
  deskflow::IStream *s = client->getStream();
  while (auto *filter = dynamic_cast<StreamFilter *>(s)) {
    s = filter->getStream();
  }
  auto *dataSocket = dynamic_cast<IDataSocket *>(s);
  if (!dataSocket) {
    return {};
  }
  std::string addr = dataSocket->getPeerAddress();
  if (addr.empty()) {
    return {};
  }
  if (addr.front() == '[') {
    auto closeBracket = addr.find(']');
    if (closeBracket != std::string::npos) {
      addr = addr.substr(1, closeBracket - 1);
    }
  } else {
    auto firstColon = addr.find(':');
    auto lastColon = addr.rfind(':');
    if (firstColon != std::string::npos && firstColon == lastColon) {
      addr = addr.substr(0, firstColon);
    }
  }
  const QString qAddr = QString::fromStdString(addr);
  if (qAddr.startsWith(QStringLiteral("::ffff:"), Qt::CaseInsensitive)) {
    return qAddr.mid(7).toStdString();
  }
  return addr;
}

std::string FileDropBridge::resolveDropTargetDirectory(
    int32_t screenX, int32_t screenY, const std::string &fallbackDir, bool *outIsDirectFolderTarget
)
{
  return InProcessDropExecutor::resolveDropTargetDirectory(screenX, screenY, fallbackDir, outIsDirectFolderTarget);
}

std::string FileDropBridge::uniqueDestinationPath(const std::string &dstDir, const std::string &relPath)
{
  return InProcessDropExecutor::uniqueDestinationPath(dstDir, relPath);
}

std::string FileDropBridge::findLocalSendCliPath()
{
  return InProcessDropExecutor::findLocalSendCliPath();
}

std::string FileDropBridge::computeTokenTag(const std::string &token)
{
  return InProcessDropExecutor::computeTokenTag(token);
}

} // namespace deskflow
