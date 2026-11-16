/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "deskflow/FileDropBridge.h"

#include "base/DirectionTypes.h"
#include "base/Log.h"
#include "deskflow/DragProtocol.h"
#include "io/StreamFilter.h"
#include "net/IDataSocket.h"
#include "server/BaseClientProxy.h"

#include <QByteArray>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QString>

#include <array>
#include <chrono>
#include <cstdlib>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace deskflow {

namespace {

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
    m_activeDragFiles.clear();
    m_activeDragToken.clear();
    m_dragTargetClient = nullptr;
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
#if defined(_WIN32)
  const bool leftButtonDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
  if (!leftButtonDown) {
    bool shouldHide = false;
    {
      std::scoped_lock lock(m_mutex);
      shouldHide = (m_visibleSentinelEdgeMask != 0);
      m_visibleSentinelEdgeMask = 0;
      if (!m_pendingFiles.empty() && (nowMs() - m_pendingTimestampMs > 500)) {
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

  const uint32_t visibleMask = computeNearEdgeMask(x, y, ax, ay, aw, ah, activeSides);
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
    cmd.activeSides = activeSides;
    exec->execute(cmd);
  }
#else
  (void)x;
  (void)y;
  (void)ax;
  (void)ay;
  (void)aw;
  (void)ah;
  (void)activeSides;
#endif
}

bool FileDropBridge::isSentinelCaptureSupportedFormat(bool hasHdrop, bool nonEmpty)
{
  return hasHdrop && nonEmpty;
}

void FileDropBridge::clearPendingDragOnMouseLeaveOrRelease()
{
  {
    std::scoped_lock lock(m_mutex);
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
  return !m_activeDragFiles.empty();
}

bool FileDropBridge::hasActiveTransfers() const
{
  if (auto *exec = executor()) {
    return exec->hasActiveTransfers();
  }
  return false;
}

void FileDropBridge::onPendingDragCaptured(std::vector<std::string> files, int sideIndex, int32_t, int32_t)
{
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

void FileDropBridge::onLeavePrimary(BaseClientProxy *dstClient)
{
  if (auto *exec = executor()) {
    DropAgentCommand hideCmd;
    hideCmd.type = DropAgentCommandType::HideSentinels;
    exec->execute(hideCmd);
  }

  std::vector<std::string> files;
  {
    std::scoped_lock lock(m_mutex);
    if (m_pendingFiles.empty() || (nowMs() - m_pendingTimestampMs) > 4000) {
      m_pendingFiles.clear();
      return;
    }
    files = std::move(m_pendingFiles);
    m_pendingFiles.clear();
  }

  if (!hasTransferableFiles(files)) {
    LOG_INFO("FileDropBridge: dragged path(s) contain no transferable files; skipping cross-screen drag");
    return;
  }

  std::string token;
  {
    std::scoped_lock lock(m_mutex);
    m_activeDragFiles = files;
    m_dragTargetClient = dstClient;
    if (dstClient) {
      token = generateAuthToken();
    }
    m_activeDragToken = token;
  }

  if (dstClient) {
    std::string msg = "DESKSEND_START:" + token;
    for (const auto &f : files) {
      msg += "\n";
      msg += QFileInfo(QString::fromStdString(f)).fileName().toStdString();
    }
    LOG_INFO(
        "FileDropBridge: leaving primary with %zu dragged file(s) to \"%s\"", files.size(),
        dstClient->getName().c_str()
    );
    dstClient->sendDragInfo(static_cast<uint32_t>(files.size()), msg.data(), msg.size());
  }
}

void FileDropBridge::onReturnToPrimary()
{
  BaseClientProxy *oldTarget = nullptr;
  std::string oldToken;
  {
    std::scoped_lock lock(m_mutex);
    if (m_activeDragFiles.empty()) {
      return;
    }
    LOG_INFO("FileDropBridge: cursor returned to primary before release; cancelling file drag");
    m_pendingFiles.clear();
    m_canCaptureDrag = false;
    m_activeDragFiles.clear();
    oldTarget = m_dragTargetClient;
    oldToken = std::move(m_activeDragToken);
    m_dragTargetClient = nullptr;
    m_activeDragToken.clear();
  }
  if (oldTarget) {
    const std::string cancelMsg = oldToken.empty() ? std::string("DESKSEND_CANCEL") : ("DESKSEND_CANCEL:" + oldToken);
    oldTarget->sendDragInfo(0, cancelMsg.data(), cancelMsg.size());
  }
}

void FileDropBridge::onClientDisconnected(BaseClientProxy *client)
{
  if (!client) {
    return;
  }
  std::scoped_lock lock(m_mutex);
  if (m_dragTargetClient == client) {
    LOG_INFO("FileDropBridge: active drag target client disconnected; clearing target pointer");
    m_dragTargetClient = nullptr;
    m_activeDragFiles.clear();
    m_activeDragToken.clear();
  }
}

void FileDropBridge::cancelActiveDrag()
{
  BaseClientProxy *oldTarget = nullptr;
  std::string oldToken;
  {
    std::scoped_lock lock(m_mutex);
    LOG_INFO("FileDropBridge: Escape pressed; cancelling file drag");
    m_pendingFiles.clear();
    m_canCaptureDrag = false;
    m_activeDragFiles.clear();
    oldTarget = m_dragTargetClient;
    oldToken = std::move(m_activeDragToken);
    m_dragTargetClient = nullptr;
    m_activeDragToken.clear();
  }
  if (oldTarget) {
    const std::string cancelMsg = oldToken.empty() ? std::string("DESKSEND_CANCEL") : ("DESKSEND_CANCEL:" + oldToken);
    oldTarget->sendDragInfo(0, cancelMsg.data(), cancelMsg.size());
  }
}

void FileDropBridge::onSecondaryLeftMouseUp(BaseClientProxy *activeClient, int32_t x, int32_t y)
{
  std::vector<std::string> files;
  std::string token;
  {
    std::scoped_lock lock(m_mutex);
    if (m_activeDragFiles.empty() || activeClient == nullptr) {
      return;
    }
    files = std::move(m_activeDragFiles);
    token = std::move(m_activeDragToken);
    m_activeDragFiles.clear();
    m_activeDragToken.clear();
    m_dragTargetClient = nullptr;
  }

  auto sendCancel = [&]() {
    const std::string cancelMsg = token.empty() ? std::string("DESKSEND_CANCEL") : ("DESKSEND_CANCEL:" + token);
    activeClient->sendDragInfo(0, cancelMsg.data(), cancelMsg.size());
  };

  if (!hasTransferableFiles(files)) {
    LOG_WARN("FileDropBridge: dragged path(s) contain no transferable files; cancelling drop");
    sendCancel();
    return;
  }

  std::string peerIp = getClientPeerIp(activeClient);
  if (peerIp.empty()) {
    LOG_WARN("FileDropBridge: could not determine peer IP for client \"%s\"", activeClient->getName().c_str());
    sendCancel();
    return;
  }

  std::string dropMsg = "DESKSEND_DROP:" + std::to_string(x) + "," + std::to_string(y);
  const std::string tag = computeTokenTag(token);
  if (!tag.empty()) {
    dropMsg += ":" + tag;
  }
  activeClient->sendDragInfo(static_cast<uint32_t>(files.size()), dropMsg.data(), dropMsg.size());

  LOG_INFO(
      "FileDropBridge: drop at (%d,%d) on \"%s\" (%s), sending %zu file(s) via LocalSend", x, y,
      activeClient->getName().c_str(), peerIp.c_str(), files.size()
  );
  if (auto *exec = executor()) {
    DropAgentCommand cmd;
    cmd.type = DropAgentCommandType::SendFiles;
    cmd.text = peerIp;
    cmd.items = std::move(files);
    cmd.authToken = std::move(token);
    exec->execute(cmd);
  }
}

void FileDropBridge::startReceiver(const std::string &serverHost)
{
  if (auto *exec = executor()) {
    DropAgentCommand cmd;
    cmd.type = DropAgentCommandType::StartReceiver;
    if (!serverHost.empty()) {
      cmd.items.push_back(serverHost);
    }
    exec->execute(cmd);
  }
}

void FileDropBridge::stopReceiver()
{
  if (auto *exec = executor()) {
    DropAgentCommand cmd;
    cmd.type = DropAgentCommandType::StopReceiver;
    exec->execute(cmd);
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
  auto *exec = executor();
  if (!exec) {
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

    const std::string startPrefix = "DESKSEND_START:";
    if (info.rfind(startPrefix, 0) == 0) {
      const size_t newlinePos = info.find('\n', startPrefix.size());
      std::string token = (newlinePos == std::string::npos)
                              ? info.substr(startPrefix.size())
                              : info.substr(startPrefix.size(), newlinePos - startPrefix.size());
      if (!token.empty() && token.back() == '\r') {
        token.pop_back();
      }
      if (!token.empty() && token.find_first_of(" \t\r\n") == std::string::npos) {
        DropAgentCommand authCmd;
        authCmd.type = DropAgentCommandType::ReceiverAuthCommand;
        authCmd.text = "GRANT " + token;
        exec->execute(authCmd);
      }
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
