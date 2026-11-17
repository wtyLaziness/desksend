// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "deskflow/DragOverlayWidgets.h"
#include "deskflow/DragProtocol.h"
#include "deskflow/DropAgentIpc.h"
#include "deskflow/DropExecutor.h"
#include "deskflow/FileDropBridge.h"
#include "base/Log.h"
#include "net/IDataSocket.h"
#include "server/BaseClientProxy.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QStringList>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <memory>
#include <thread>

namespace {

class MockDragDataSocket final : public IDataSocket
{
public:
  explicit MockDragDataSocket(const std::string &peerAddr) : IDataSocket(nullptr)
  {
    setPeerAddress(peerAddr);
  }

  void bind(const NetworkAddress &) override {}
  void close() override {}
  void *getEventTarget() const override { return const_cast<MockDragDataSocket *>(this); }
  void connect(const NetworkAddress &) override {}
  bool isFatal() const override { return false; }
  uint32_t read(void *, uint32_t) override { return 0; }
  void write(const void *, uint32_t) override {}
  void flush() override {}
  void shutdownInput() override {}
  void shutdownOutput() override {}
  bool isReady() const override { return false; }
  uint32_t getSize() const override { return 0; }
};

class MockDragClientProxy final : public BaseClientProxy
{
public:
  explicit MockDragClientProxy(const std::string &name, const std::string &peerAddress = {})
      : BaseClientProxy(name)
  {
    if (!peerAddress.empty()) {
      m_socket = std::make_unique<MockDragDataSocket>(peerAddress);
    }
  }

  std::vector<std::pair<uint32_t, std::string>> dragMessages;
  std::unique_ptr<MockDragDataSocket> m_socket;

  void *getEventTarget() const override { return nullptr; }
  bool getClipboard(ClipboardID, IClipboard *) const override { return false; }
  void getShape(int32_t &, int32_t &, int32_t &, int32_t &) const override {}
  void getCursorPos(int32_t &, int32_t &) const override {}
  void enter(int32_t, int32_t, uint32_t, KeyModifierMask, bool) override {}
  bool leave() override { return true; }
  void setClipboard(ClipboardID, const IClipboard *) override {}
  void grabClipboard(ClipboardID) override {}
  void setClipboardDirty(ClipboardID, bool) override {}
  void keyDown(KeyID, KeyModifierMask, KeyButton, const std::string &) override {}
  void keyRepeat(KeyID, KeyModifierMask, int32_t, KeyButton, const std::string &) override {}
  void keyUp(KeyID, KeyModifierMask, KeyButton) override {}
  void mouseDown(ButtonID) override {}
  void mouseUp(ButtonID) override {}
  void mouseMove(int32_t, int32_t) override {}
  void mouseRelativeMove(int32_t, int32_t) override {}
  void mouseWheel(int32_t, int32_t) override {}
  void screensaver(bool) override {}
  void resetOptions() override {}
  void setOptions(const OptionsList &) override {}
  void sendDragInfo(uint32_t fileCount, const char *info, size_t size) override
  {
    dragMessages.emplace_back(fileCount, std::string(info, size));
  }
  void fileChunkSending(uint8_t, char *, size_t) override {}
  std::string getSecureInputApp() const override { return {}; }
  void secureInputNotification(const std::string &) const override {}
  deskflow::IStream *getStream() const override { return m_socket.get(); }
};

struct RecordedExecutorState
{
  std::vector<deskflow::DropAgentCommand> commands;
  deskflow::IDropExecutorHost *host = nullptr;
  bool hoverVisible = false;
  uint32_t hoverFileCount = 0;
  QPoint hoverCursorPos;
  deskflow::TransferStatusOverlay::Phase receiverPhase = deskflow::TransferStatusOverlay::Phase::Idle;
  uint32_t preparingFileCount = 0;
  bool activeTransfers = false;
  deskflow::TransferStatusOverlay rxOverlay{true, true};
  deskflow::TransferStatusOverlay txOverlay{false, true};

  void clearCommands()
  {
    commands.clear();
  }

  const deskflow::DropAgentCommand *findCommand(deskflow::DropAgentCommandType type) const
  {
    for (const auto &cmd : commands) {
      if (cmd.type == type) {
        return &cmd;
      }
    }
    return nullptr;
  }

  size_t countCommands(deskflow::DropAgentCommandType type) const
  {
    size_t n = 0;
    for (const auto &cmd : commands) {
      if (cmd.type == type) {
        ++n;
      }
    }
    return n;
  }
};

class RecordingDropExecutor final : public deskflow::IDropExecutor
{
public:
  explicit RecordingDropExecutor(std::shared_ptr<RecordedExecutorState> state) : m_state(std::move(state)) {}

  void setHost(deskflow::IDropExecutorHost *host) override
  {
    m_state->host = host;
  }

  void execute(const deskflow::DropAgentCommand &cmd) override
  {
    m_state->commands.push_back(cmd);
    switch (cmd.type) {
    case deskflow::DropAgentCommandType::ShowHover:
      m_state->hoverVisible = true;
      m_state->hoverFileCount = cmd.fileCount;
      m_state->hoverCursorPos = QPoint(cmd.x, cmd.y);
      break;
    case deskflow::DropAgentCommandType::UpdateHoverCursor:
      m_state->hoverCursorPos = QPoint(cmd.x, cmd.y);
      break;
    case deskflow::DropAgentCommandType::HideHover:
      m_state->hoverVisible = false;
      break;
    case deskflow::DropAgentCommandType::BeginPreparing:
      m_state->receiverPhase = deskflow::TransferStatusOverlay::Phase::Preparing;
      m_state->preparingFileCount = cmd.fileCount;
      break;
    case deskflow::DropAgentCommandType::CancelPreparing:
      m_state->receiverPhase = deskflow::TransferStatusOverlay::Phase::Idle;
      m_state->preparingFileCount = 0;
      break;
    default:
      break;
    }
  }

  bool isHoverVisible() const override { return m_state->hoverVisible; }
  uint32_t hoverFileCount() const override { return m_state->hoverFileCount; }
  QPoint hoverCursorPos() const override { return m_state->hoverCursorPos; }
  deskflow::TransferStatusOverlay::Phase receiverOverlayPhase() const override { return m_state->receiverPhase; }
  uint32_t receiverPreparingFileCount() const override { return m_state->preparingFileCount; }
  deskflow::TransferStatusOverlay &overlayFor(bool receiving) override
  {
    return receiving ? m_state->rxOverlay : m_state->txOverlay;
  }
  bool hasActiveTransfers() const override { return m_state->activeTransfers; }

private:
  std::shared_ptr<RecordedExecutorState> m_state;
};

} // namespace

class FileDropBridgeTests : public QObject
{
  Q_OBJECT
private:
  Log m_log;

private Q_SLOTS:
  void initTestCase()
  {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
  }

  void cleanupTestCase()
  {
    auto &bridge = deskflow::FileDropBridge::instance();
    bridge.resetAllStateForTesting();
    bridge.shutdownSentinels();
    bridge.stopReceiver();
    bridge.setDropExecutorForTesting(std::make_unique<RecordingDropExecutor>(std::make_shared<RecordedExecutorState>()));
  }

  void uniqueDestinationPathPreservesDotfilesAndMultiExtensions()
  {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const std::string dstDir = tempDir.path().toStdString();

    // Non-existing dotfile keeps exact name
    const QString gitignorePath =
        QString::fromStdString(deskflow::FileDropBridge::uniqueDestinationPath(dstDir, ".gitignore"));
    QCOMPARE(QFileInfo(gitignorePath).fileName(), QStringLiteral(".gitignore"));

    // Create collision for .gitignore -> ".gitignore (1)" (not " (1).gitignore")
    {
      QFile f(gitignorePath);
      QVERIFY(f.open(QIODevice::WriteOnly));
    }
    const QString gitignoreConflict =
        QString::fromStdString(deskflow::FileDropBridge::uniqueDestinationPath(dstDir, ".gitignore"));
    QCOMPARE(QFileInfo(gitignoreConflict).fileName(), QStringLiteral(".gitignore (1)"));

    // Multi-extension file keeps middle extension
    const QString tarGzPath =
        QString::fromStdString(deskflow::FileDropBridge::uniqueDestinationPath(dstDir, "archive.tar.gz"));
    {
      QFile f(tarGzPath);
      QVERIFY(f.open(QIODevice::WriteOnly));
    }
    const QString tarGzConflict =
        QString::fromStdString(deskflow::FileDropBridge::uniqueDestinationPath(dstDir, "archive.tar.gz"));
    QCOMPARE(QFileInfo(tarGzConflict).fileName(), QStringLiteral("archive.tar (1).gz"));

    // Nested relative path creates parent dir and avoids collision
    const QString nestedPath =
        QString::fromStdString(deskflow::FileDropBridge::uniqueDestinationPath(dstDir, "sub/README"));
    QCOMPARE(QDir(tempDir.path()).relativeFilePath(nestedPath), QStringLiteral("sub/README"));
    {
      QFile f(nestedPath);
      QVERIFY(f.open(QIODevice::WriteOnly));
    }
    const QString nestedConflict =
        QString::fromStdString(deskflow::FileDropBridge::uniqueDestinationPath(dstDir, "sub/README"));
    QCOMPARE(QDir(tempDir.path()).relativeFilePath(nestedConflict), QStringLiteral("sub/README (1)"));
  }

  void transferStateDefaultsAndDragCaptureGuards()
  {
    auto &bridge = deskflow::FileDropBridge::instance();
    QVERIFY(!bridge.hasActiveTransfers());
    QVERIFY(!bridge.hasActiveDragFiles());

    bridge.onPrimaryLeftMouseDown();
    QVERIFY(bridge.setPendingDragFiles({"C:/tmp/a.txt"}));
    QVERIFY(bridge.hasPendingDragFiles());
    // Duplicate capture without a new button down is rejected
    QVERIFY(!bridge.setPendingDragFiles({"C:/tmp/b.txt"}));

    bridge.cancelActiveDrag();
    QVERIFY(!bridge.hasPendingDragFiles());
    QVERIFY(!bridge.hasActiveDragFiles());
  }

  void emptyFolderSkippedBeforeStart()
  {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString emptySubdir = QDir(tempDir.path()).filePath(QStringLiteral("empty_folder"));
    QVERIFY(QDir().mkpath(emptySubdir));

    auto &bridge = deskflow::FileDropBridge::instance();
    bridge.cancelActiveDrag();
    MockDragClientProxy mockClient("client");

    bridge.onPrimaryLeftMouseDown();
    QVERIFY(bridge.setPendingDragFiles({emptySubdir.toStdString()}));
    bridge.onLeavePrimary(&mockClient);
    QVERIFY(mockClient.dragMessages.empty());
    QVERIFY(!bridge.hasActiveDragFiles());

    const QString filledSubdir = QDir(tempDir.path()).filePath(QStringLiteral("filled_folder"));
    QVERIFY(QDir().mkpath(filledSubdir));
    {
      QFile f(QDir(filledSubdir).filePath(QStringLiteral("payload.txt")));
      QVERIFY(f.open(QIODevice::WriteOnly));
      f.write("data");
    }

    bridge.onPrimaryLeftMouseDown();
    QVERIFY(bridge.setPendingDragFiles({filledSubdir.toStdString()}));
    bridge.onLeavePrimary(&mockClient);
    QCOMPARE(mockClient.dragMessages.size(), size_t{1});
    QCOMPARE(mockClient.dragMessages[0].first, 1u);
    QVERIFY(mockClient.dragMessages[0].second.rfind("DESKSEND_START:", 0) == 0);
    QVERIFY(bridge.hasActiveDragFiles());

    bridge.onClientDisconnected(&mockClient);
    bridge.cancelActiveDrag();
    QVERIFY(!bridge.hasActiveDragFiles());
  }

  void tokenTagMatchesPendingTargetsAndCancelsSafely()
  {
    const std::string tokenA = "alpha-token-123";
    const std::string tokenB = "beta-token-456";
    const std::string tokenC = "gamma-token-789";
    const std::string tagA = deskflow::FileDropBridge::computeTokenTag(tokenA);
    const std::string tagB = deskflow::FileDropBridge::computeTokenTag(tokenB);
    const std::string tagC = deskflow::FileDropBridge::computeTokenTag(tokenC);
    QCOMPARE(tagA.size(), size_t{16});
    QCOMPARE(
        QByteArray::fromStdString(tagA),
        QCryptographicHash::hash(QByteArray::fromStdString(tokenA), QCryptographicHash::Sha256).toHex().left(16)
    );
    QVERIFY(tagA != tagB);
    QVERIFY(tagB != tagC);

    QTemporaryDir dirA, dirB, dirC, fallback;
    QVERIFY(dirA.isValid() && dirB.isValid() && dirC.isValid() && fallback.isValid());

    const auto steadyNowMs = []() -> uint64_t {
      using namespace std::chrono;
      return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
    };

    auto &bridge = deskflow::FileDropBridge::instance();
    bridge.stopReceiver();
    auto *exec = bridge.inProcessExecutorForTesting();
    QVERIFY(exec != nullptr);
    {
      std::scoped_lock lock(exec->m_mutex);
      exec->m_receiverDesired = true;
      exec->m_defaultDropFallbackDir = fallback.path().toStdString();
    }
    bridge.onClientReceiveDragInfo(1, "DESKSEND_DROP:999999,999999:" + tagA);
    {
      std::scoped_lock lock(exec->m_mutex);
      QCOMPARE(exec->m_pendingDropTargets.size(), size_t{1});
      QCOMPARE(exec->m_pendingDropTargets.back().tokenTag, tagA);
      exec->m_pendingDropTargets.back().dir = dirA.path().toStdString();
      const uint64_t t = steadyNowMs();
      exec->m_pendingDropTargets.push_back({dirB.path().toStdString(), true, t, tagB});
      exec->m_pendingDropTargets.push_back({dirC.path().toStdString(), false, t, tagC});
    }

    // Cancel token A only: removes tag A while leaving tag B and tag C intact
    bridge.onClientReceiveDragInfo(0, "DESKSEND_CANCEL:" + tokenA);
    {
      std::scoped_lock lock(exec->m_mutex);
      QCOMPARE(exec->m_pendingDropTargets.size(), size_t{2});
      QCOMPARE(exec->m_pendingDropTargets[0].tokenTag, tagB);
      QCOMPARE(exec->m_pendingDropTargets[1].tokenTag, tagC);
      QVERIFY(!exec->m_pendingReceiverAuthLines.empty());
      QCOMPARE(exec->m_pendingReceiverAuthLines.back(), std::string("REVOKE " + tokenA));
      // Refresh timestamps relative to steady_clock so neither expires
      const uint64_t now = steadyNowMs();
      exec->m_pendingDropTargets[0].timestampMs = now;
      exec->m_pendingDropTargets[1].timestampMs = now;
    }

    // Session for token C arrives even though orphaned tag B never started: jumps to C and discards B
    bridge.handleReceiveSessionStarted(2, tagC);
    {
      std::scoped_lock lock(exec->m_mutex);
      QCOMPARE(exec->m_activeSessionTarget.dir, dirC.path().toStdString());
      QCOMPARE(exec->m_activeSessionTarget.tokenTag, tagC);
      QVERIFY(!exec->m_activeSessionTarget.copyToClipboard);
      QVERIFY(exec->m_pendingDropTargets.empty());
    }

    bridge.stopReceiver();
  }

  void nonFileDragDoesNotSetPendingFiles()
  {
    QVERIFY(!deskflow::FileDropBridge::isSentinelCaptureSupportedFormat(false, true));
    QVERIFY(!deskflow::FileDropBridge::isSentinelCaptureSupportedFormat(true, false));
    QVERIFY(deskflow::FileDropBridge::isSentinelCaptureSupportedFormat(true, true));

    auto &bridge = deskflow::FileDropBridge::instance();
    bridge.onPrimaryLeftMouseDown();
    QVERIFY(!bridge.setPendingDragFiles({}));
    QVERIFY(!bridge.hasPendingDragFiles());

    bridge.clearPendingDragOnMouseLeaveOrRelease();
    QVERIFY(!bridge.hasPendingDragFiles());
  }

  void onLeftMouseUp_hidesSentinelAndClearsCancellationFlags()
  {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString filePath = tempDir.filePath("sample.txt");
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("data");
    file.close();

    auto &bridge = deskflow::FileDropBridge::instance();
    bridge.onPrimaryLeftMouseDown();
    QVERIFY(bridge.setPendingDragFiles({filePath.toStdString()}));
    QVERIFY(bridge.hasPendingDragFiles());

    {
      std::scoped_lock lock(bridge.m_mutex);
      bridge.m_visibleSentinelEdgeMask = 0x0F;
      bridge.m_dragCancellationInProgress = true;
      bridge.m_dragCancellationComplete = true;
    }

    bridge.onLeftMouseUp(false);
    QVERIFY(!bridge.hasPendingDragFiles());
    QVERIFY(!bridge.isDragHandoffInProgress());
    {
      std::scoped_lock lock(bridge.m_mutex);
      QCOMPARE(bridge.m_visibleSentinelEdgeMask, 0u);
      QVERIFY(!bridge.m_sourceButtonNeedsRelease);
      QVERIFY(!bridge.m_dragCancellationInProgress);
      QVERIFY(!bridge.m_dragCancellationComplete);
    }
  }

  void dragProtocol_serializesParsesEdgesAndClipsCorners()
  {
    using deskflow::DragEdgeSegment;
    using deskflow::DragEdgeSide;

    const std::vector<DragEdgeSegment> raw = {
        {DragEdgeSide::Right, 0, 1080},
        {DragEdgeSide::Top, 0, 16},
        {DragEdgeSide::Top, 1904, 1920},
        {DragEdgeSide::Left, 200, 500},
        {DragEdgeSide::Bottom, 0, 1920},
    };
    const auto clipped = deskflow::clipEdgeSegmentsForCorners(raw, 1920, 1080, 16);
    QCOMPARE(clipped.size(), static_cast<size_t>(3));
    QCOMPARE(clipped[0].side, DragEdgeSide::Right);
    QCOMPARE(clipped[0].start, 16);
    QCOMPARE(clipped[0].end, 1064);
    QCOMPARE(clipped[1].side, DragEdgeSide::Left);
    QCOMPARE(clipped[1].start, 200);
    QCOMPARE(clipped[1].end, 500);
    QCOMPARE(clipped[2].side, DragEdgeSide::Bottom);
    QCOMPARE(clipped[2].start, 16);
    QCOMPARE(clipped[2].end, 1904);

    const std::string wireEdges = deskflow::serializeDragEdgesMessage(clipped);
    std::vector<DragEdgeSegment> parsedEdges;
    QVERIFY(deskflow::parseDragEdgesMessage(wireEdges, parsedEdges));
    QCOMPARE(parsedEdges, clipped);

    std::vector<DragEdgeSegment> emptyEdges;
    QVERIFY(deskflow::parseDragEdgesMessage(deskflow::serializeDragEdgesMessage({}), emptyEdges));
    QVERIFY(emptyEdges.empty());

    const std::vector<std::string> peers = {"192.168.1.5", "2001:db8::42"};
    std::vector<std::string> parsedPeers;
    QVERIFY(deskflow::parseDragPeersMessage(deskflow::serializeDragPeersMessage(peers), parsedPeers));
    QCOMPARE(parsedPeers, peers);

    DragEdgeSide parsedSide = DragEdgeSide::Left;
    int32_t x = 0;
    int32_t y = 0;
    QVERIFY(deskflow::parseClientPreCaptureMessage(
        deskflow::serializeClientPreCaptureMessage(DragEdgeSide::Bottom, 640, 1079), parsedSide, x, y
    ));
    QCOMPARE(parsedSide, DragEdgeSide::Bottom);
    QCOMPARE(x, 640);
    QCOMPARE(y, 1079);

    QVERIFY(deskflow::isClientPreCaptureClearMessage(deskflow::kMsgDragClientPreCaptureClear));
    QVERIFY(deskflow::isCommitHandoffMessage(deskflow::kMsgDragCommitHandoff));
    QVERIFY(deskflow::isHoverLeaveMessage(deskflow::kMsgDragHoverLeave));

    QVERIFY(deskflow::parseHoverEnterMessage(deskflow::serializeHoverEnterMessage(320, 480), x, y));
    QCOMPARE(x, 320);
    QCOMPARE(y, 480);

    std::string token;
    std::string target;
    const std::string wireSendDropIpv6 = deskflow::serializeSendDropMessage(120, 240, "secretTok99", "2001:db8::42");
    QVERIFY(deskflow::parseSendDropMessage(wireSendDropIpv6, x, y, token, target));
    QCOMPARE(x, 120);
    QCOMPARE(y, 240);
    QCOMPARE(token, std::string("secretTok99"));
    QCOMPARE(target, std::string("2001:db8::42"));

    QVERIFY(deskflow::parseDragCancelMessage("DESKSEND_CANCEL", token));
    QVERIFY(token.empty());
    QVERIFY(deskflow::parseDragCancelMessage("DESKSEND_CANCEL:tokA", token));
    QCOMPARE(token, std::string("tokA"));
    QVERIFY(deskflow::parseSendCancelMessage("DESKSEND_SEND_CANCEL", token));
    QVERIFY(token.empty());
    QVERIFY(deskflow::parseSendCancelMessage("DESKSEND_SEND_CANCEL:tokB", token));
    QCOMPARE(token, std::string("tokB"));

    const std::string redactedSendDrop = deskflow::redactDragLogPayload(wireSendDropIpv6);
    QCOMPARE(redactedSendDrop, std::string("DESKSEND_SEND_DROP:120,240:<redacted>:2001:db8::42"));
    QCOMPARE(redactedSendDrop.find("secretTok99"), std::string::npos);
    QCOMPARE(deskflow::redactDragLogPayload("DESKSEND_START:secretA"), std::string("DESKSEND_START:<redacted>"));
    QCOMPARE(deskflow::redactDragLogPayload("DESKSEND_CANCEL:secretB"), std::string("DESKSEND_CANCEL:<redacted>"));
    QCOMPARE(
        deskflow::redactDragLogPayload("DESKSEND_SEND_CANCEL:secretC"), std::string("DESKSEND_SEND_CANCEL:<redacted>")
    );
  }

  void dragHoverIndicator_clampsToScreenEdgesAndFormatsCount()
  {
    QCOMPARE(
        deskflow::formatDragHoverText(3), QCoreApplication::translate("DragHoverIndicator", "Drop to receive 3 file(s)")
    );
    QCOMPARE(
        deskflow::formatDragHoverText(1), QCoreApplication::translate("DragHoverIndicator", "Drop to receive 1 file(s)")
    );

    const QRect bounds(0, 0, 1920, 1080);
    const QSize capsuleSize(220, 36);
    const QPoint normal = deskflow::clampDragHoverPosition(QPoint(500, 400), capsuleSize, bounds);
    QCOMPARE(normal, QPoint(518, 422));

    const QPoint bottomRight = deskflow::clampDragHoverPosition(QPoint(1915, 1075), capsuleSize, bounds);
    QCOMPARE(bottomRight.x(), 1920 - 220);
    QCOMPARE(bottomRight.y(), 1080 - 36);

    const QPoint negativeOrigin =
        deskflow::clampDragHoverPosition(QPoint(-1950, -10), capsuleSize, QRect(-1920, 0, 1920, 1080));
    QCOMPARE(negativeOrigin.x(), -1920);
    QCOMPARE(negativeOrigin.y(), 12);

    deskflow::DragHoverIndicator indicator;
    indicator.showHover(4, 100, 200);
    QVERIFY(indicator.isVisible());
    QCOMPARE(indicator.fileCount(), 4u);
    QCOMPARE(indicator.cursorPos(), QPoint(100, 200));
    indicator.updateCursor(250, 350);
    QCOMPARE(indicator.cursorPos(), QPoint(250, 350));
    indicator.hideHover();
    QVERIFY(!indicator.isVisible());
  }

  void dragHoverAndTransferStatus_transitionsOnStartDropCancel()
  {
    QTemporaryDir fallbackDir;
    QVERIFY(fallbackDir.isValid());
    qputenv("DESKSEND_DEFAULT_DROP_DIR", fallbackDir.path().toUtf8());

    auto &bridge = deskflow::FileDropBridge::instance();
    bridge.resetAllStateForTesting();
    bridge.stopReceiver();
    bridge.onClientReceiveDragInfo(2, "DESKSEND_START:tokHover1");
    QVERIFY(!bridge.isClientHoverVisible());

    bridge.onClientReceiveDragInfo(2, "DESKSEND_HOVER_ENTER:120,240");
    QVERIFY(bridge.isClientHoverVisible());
    QCOMPARE(bridge.clientHoverFileCount(), 2u);
    QCOMPARE(bridge.clientHoverCursorPos(), QPoint(120, 240));

    bridge.onClientMouseMove(420, 560);
    QCOMPARE(bridge.clientHoverCursorPos(), QPoint(420, 560));

    bridge.onClientReceiveDragInfo(0, "DESKSEND_HOVER_LEAVE");
    QVERIFY(!bridge.isClientHoverVisible());
    QCOMPARE(bridge.receiverOverlayPhase(), deskflow::TransferStatusOverlay::Phase::Idle);

    bridge.onClientReceiveDragInfo(3, "DESKSEND_START:tokHover2");
    bridge.onClientReceiveDragInfo(3, "DESKSEND_HOVER_ENTER:50,60");
    QVERIFY(bridge.isClientHoverVisible());

    const QString tag2 = QString::fromStdString(deskflow::FileDropBridge::computeTokenTag("tokHover2"));
    bridge.onClientReceiveDragInfo(3, "DESKSEND_DROP:300,400:" + tag2.toStdString());
    QVERIFY(!bridge.isClientHoverVisible());
    QCOMPARE(bridge.receiverOverlayPhase(), deskflow::TransferStatusOverlay::Phase::Preparing);
    QCOMPARE(bridge.receiverPreparingFileCount(), 3u);

    bridge.onClientReceiveDragInfo(0, "DESKSEND_CANCEL:tokHover2");
    QVERIFY(!bridge.isClientHoverVisible());
    QCOMPARE(bridge.receiverOverlayPhase(), deskflow::TransferStatusOverlay::Phase::Idle);

    // Also test full Preparing -> Active -> Finished progression (including out-of-order FINISHED + FILE completion)
    QTemporaryDir stagingTemp;
    QVERIFY(stagingTemp.isValid());
    const QString stagedPath = stagingTemp.filePath(QStringLiteral("received.txt"));
    {
      QFile stagedFile(stagedPath);
      QVERIFY(stagedFile.open(QIODevice::WriteOnly));
      stagedFile.write("payload");
    }

    bridge.onClientReceiveDragInfo(1, "DESKSEND_START:tokHover3");
    const QString tag3 = QString::fromStdString(deskflow::FileDropBridge::computeTokenTag("tokHover3"));
    bridge.onClientReceiveDragInfo(1, "DESKSEND_DROP:300,400:" + tag3.toStdString());
    QCOMPARE(bridge.receiverOverlayPhase(), deskflow::TransferStatusOverlay::Phase::Preparing);
    bridge.handleReceiveSessionStarted(1, tag3.toStdString());
    QCOMPARE(bridge.receiverOverlayPhase(), deskflow::TransferStatusOverlay::Phase::Active);
    bridge.handleReceiveSessionFinished(false);
    QCOMPARE(bridge.receiverOverlayPhase(), deskflow::TransferStatusOverlay::Phase::Active);
    auto *inProcExec = bridge.inProcessExecutorForTesting();
    QVERIFY(inProcExec != nullptr);
    inProcExec->handleReceivedFileLine(stagedPath.toStdString());
    QCOMPARE(bridge.receiverOverlayPhase(), deskflow::TransferStatusOverlay::Phase::Finished);
    QVERIFY(QFileInfo::exists(fallbackDir.filePath(QStringLiteral("received.txt"))));

    qunsetenv("DESKSEND_DEFAULT_DROP_DIR");
  }

  void dropAgentIpc_roundTripsAllCommandsAndEvents()
  {
    using namespace deskflow;

    const std::vector<DropAgentCommand> commands = {
        [] {
          DropAgentCommand c;
          c.type = DropAgentCommandType::UpdatePrimaryShape;
          c.x = 10;
          c.y = 20;
          c.aw = 1920;
          c.ah = 1080;
          return c;
        }(),
        [] {
          DropAgentCommand c;
          c.type = DropAgentCommandType::PrimaryMouseMove;
          c.x = 500;
          c.y = 600;
          c.aw = 1920;
          c.ah = 1080;
          c.activeSides = 0x06;
          c.edgeSegments = {{DragEdgeSide::Right, 16, 1064}};
          return c;
        }(),
        [] {
          DropAgentCommand c;
          c.type = DropAgentCommandType::PrimaryLeftMouseDown;
          return c;
        }(),
        [] {
          DropAgentCommand c;
          c.type = DropAgentCommandType::LeftMouseUp;
          c.flag = true;
          return c;
        }(),
        [] {
          DropAgentCommand c;
          c.type = DropAgentCommandType::HideSentinels;
          return c;
        }(),
        [] {
          DropAgentCommand c;
          c.type = DropAgentCommandType::ShutdownSentinels;
          return c;
        }(),
        [] {
          DropAgentCommand c;
          c.type = DropAgentCommandType::CancelNativeDrag;
          c.sideIndex = 1;
          return c;
        }(),
        [] {
          DropAgentCommand c;
          c.type = DropAgentCommandType::ShowHover;
          c.fileCount = 5;
          c.x = 120;
          c.y = 240;
          return c;
        }(),
        [] {
          DropAgentCommand c;
          c.type = DropAgentCommandType::UpdateHoverCursor;
          c.x = 300;
          c.y = 400;
          return c;
        }(),
        [] {
          DropAgentCommand c;
          c.type = DropAgentCommandType::HideHover;
          return c;
        }(),
        [] {
          DropAgentCommand c;
          c.type = DropAgentCommandType::BeginPreparing;
          c.fileCount = 3;
          return c;
        }(),
        [] {
          DropAgentCommand c;
          c.type = DropAgentCommandType::CancelPreparing;
          return c;
        }(),
        [] {
          DropAgentCommand c;
          c.type = DropAgentCommandType::TransferBegin;
          c.flag = true;
          c.text = "Receiving 3 file(s)";
          return c;
        }(),
        [] {
          DropAgentCommand c;
          c.type = DropAgentCommandType::TransferProgress;
          c.flag = false;
          c.done = 1024;
          c.total = 4096;
          return c;
        }(),
        [] {
          DropAgentCommand c;
          c.type = DropAgentCommandType::TransferFinish;
          c.flag = true;
          c.dragActive = true;
          c.success = true;
          c.text = "Done";
          return c;
        }(),
        [] {
          DropAgentCommand c;
          c.type = DropAgentCommandType::StartReceiver;
          c.items = {"192.168.1.10", "2001:db8::5"};
          return c;
        }(),
        [] {
          DropAgentCommand c;
          c.type = DropAgentCommandType::StopReceiver;
          return c;
        }(),
        [] {
          DropAgentCommand c;
          c.type = DropAgentCommandType::ReceiverAuthCommand;
          c.text = "GRANT tok123";
          return c;
        }(),
        [] {
          DropAgentCommand c;
          c.type = DropAgentCommandType::DropTarget;
          c.fileCount = 2;
          c.x = 777;
          c.y = 888;
          c.text = "tag456";
          return c;
        }(),
        [] {
          DropAgentCommand c;
          c.type = DropAgentCommandType::CancelDropToken;
          c.text = "tag456";
          return c;
        }(),
        [] {
          DropAgentCommand c;
          c.type = DropAgentCommandType::SendFiles;
          c.text = "192.168.1.99";
          c.items = {"C:/a.txt", "D:/dir/b.txt"};
          c.authToken = "secret99";
          return c;
        }(),
        [] {
          DropAgentCommand c;
          c.type = DropAgentCommandType::CopyFilesToClipboard;
          c.items = {"C:/rx.txt"};
          return c;
        }(),
        [] {
          DropAgentCommand c;
          c.type = DropAgentCommandType::SetDragActive;
          c.dragActive = true;
          return c;
        }(),
        [] {
          DropAgentCommand c;
          c.type = DropAgentCommandType::Shutdown;
          return c;
        }(),
    };

    for (const auto &cmd : commands) {
      const std::string json = serializeDropAgentCommand(cmd);
      QCOMPARE(json.find('\n'), std::string::npos);
      const auto parsed = deserializeDropAgentCommand(json);
      QVERIFY(parsed.has_value());
      QCOMPARE(*parsed, cmd);
    }

    const std::vector<DropAgentEvent> events = {
        {DropAgentEventType::Ready},
        [] {
          DropAgentEvent e;
          e.type = DropAgentEventType::PendingDragCaptured;
          e.files = {"C:/foo.txt", "C:/bar.txt"};
          e.sideIndex = 1;
          e.x = 1919;
          e.y = 500;
          return e;
        }(),
        [] {
          DropAgentEvent e;
          e.type = DropAgentEventType::DragCancellationCompleted;
          e.flag = true;
          return e;
        }(),
        {DropAgentEventType::PendingDragCleared},
        [] {
          DropAgentEvent e;
          e.type = DropAgentEventType::ActiveTransfersChanged;
          e.flag = true;
          return e;
        }(),
        [] {
          DropAgentEvent e;
          e.type = DropAgentEventType::SendCompleted;
          e.text = "192.168.1.99";
          e.flag = true;
          return e;
        }(),
        [] {
          DropAgentEvent e;
          e.type = DropAgentEventType::LogMessage;
          e.level = 2;
          e.text = "hello agent";
          return e;
        }(),
    };

    for (const auto &ev : events) {
      const std::string json = serializeDropAgentEvent(ev);
      QCOMPARE(json.find('\n'), std::string::npos);
      const auto parsed = deserializeDropAgentEvent(json);
      QVERIFY(parsed.has_value());
      QCOMPARE(*parsed, ev);
    }

    QVERIFY(!deserializeDropAgentCommand("").has_value());
    QVERIFY(!deserializeDropAgentCommand("{bad json").has_value());
    QVERIFY(!deserializeDropAgentEvent("").has_value());
    QVERIFY(!deserializeDropAgentEvent("{\"type\":\"unknown\"}").has_value());
  }

  void dropExecutor_defersReceiverRestartWhileTransferOrDragActive()
  {
    deskflow::ReceiverRestartPolicy policy;

    const auto init = policy.requestHosts({"192.168.1.10"}, false, false);
    QVERIFY(init.has_value());
    QCOMPARE(*init, (std::vector<std::string>{"192.168.1.10"}));
    QVERIFY(!policy.hasDeferredRestart());

    // Duplicate same hosts -> no restart
    QVERIFY(!policy.requestHosts({"::ffff:192.168.1.10", "192.168.1.10"}, false, false).has_value());

    // Peer update during active drag must defer
    QVERIFY(!policy.requestHosts({"192.168.1.10", "192.168.1.20"}, true, false).has_value());
    QVERIFY(policy.hasDeferredRestart());
    QCOMPARE(policy.activeHosts(), (std::vector<std::string>{"192.168.1.10"}));

    // Drag ended, transfer still running -> still deferred
    QVERIFY(!policy.onActivityChanged(false, true).has_value());
    QVERIFY(policy.hasDeferredRestart());

    // Transfer finished -> deferred restart now flushes
    const auto flushed = policy.onActivityChanged(false, false);
    QVERIFY(flushed.has_value());
    QCOMPARE(*flushed, (std::vector<std::string>{"192.168.1.10", "192.168.1.20"}));
    QVERIFY(!policy.hasDeferredRestart());

    // Deferring then reverting to same active hosts elides unnecessary restart
    QVERIFY(!policy.requestHosts({"192.168.1.10"}, false, true).has_value());
    QVERIFY(policy.hasDeferredRestart());
    QVERIFY(!policy.requestHosts({"192.168.1.10", "192.168.1.20"}, false, true).has_value());
    QVERIFY(!policy.onActivityChanged(false, false).has_value());
    QVERIFY(!policy.hasDeferredRestart());
  }

  void clientToPrimary_twoPhaseHandoffAndDropOnPrimary()
  {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString samplePath = QDir(tempDir.path()).filePath(QStringLiteral("payload.txt"));
    {
      QFile f(samplePath);
      QVERIFY(f.open(QIODevice::WriteOnly));
      f.write("hello");
    }

    auto &bridge = deskflow::FileDropBridge::instance();
    struct ResetGuard
    {
      deskflow::FileDropBridge &b;
      ~ResetGuard()
      {
        b.resetAllStateForTesting();
        b.setDropExecutorForTesting(nullptr);
      }
    } guard{bridge};
    bridge.resetAllStateForTesting();

    auto clientState = std::make_shared<RecordedExecutorState>();
    auto serverState = std::make_shared<RecordedExecutorState>();

    // --- Phase 1: Client A captures files near edge & notifies Server ---
    std::vector<std::pair<uint32_t, std::string>> clientToServerMsgs;
    bridge.setDropExecutorForTesting(std::make_unique<RecordingDropExecutor>(clientState));
    bridge.startClientReceiver("10.0.0.1");
    bridge.setClientDragMessageSender([&](uint32_t count, const std::string &info) {
      clientToServerMsgs.emplace_back(count, info);
    });
    bridge.onClientReceiveDragInfo(
        0, deskflow::serializeDragEdgesMessage({{deskflow::DragEdgeSide::Right, 16, 1064}})
    );
    bridge.onClientMouseLeftDown();
    QVERIFY(clientState->host != nullptr);
    clientState->host->onPendingDragCaptured({samplePath.toStdString()}, 1, 1919, 500);

    QCOMPARE(clientState->countCommands(deskflow::DropAgentCommandType::NudgeAcrossEdge), size_t{0});
    QCOMPARE(clientToServerMsgs.size(), size_t{1});
    QCOMPARE(clientToServerMsgs[0].first, 1u);
    QCOMPARE(clientToServerMsgs[0].second, std::string("DESKSEND_CLIENT_PRECAPTURE:R:1919,500"));
    clientState->clearCommands();

    // --- Phase 2: Server receives PreCapture and initiates two-phase handoff ---
    MockDragClientProxy mockClientA("clientA", "10.0.0.11");
    bridge.setClientDragMessageSender({});
    bridge.setDropExecutorForTesting(std::make_unique<RecordingDropExecutor>(serverState));
    bridge.onServerReceiveClientDragInfo(&mockClientA, clientToServerMsgs[0].first, clientToServerMsgs[0].second);
    QVERIFY(bridge.canUnlockEdgeSwitchForDrag(&mockClientA));

    bridge.prepareScreenSwitch(&mockClientA, nullptr);
    QCOMPARE(mockClientA.dragMessages.size(), size_t{1});
    QCOMPARE(mockClientA.dragMessages[0].second, std::string(deskflow::kMsgDragCommitHandoff));

    bridge.onScreenSwitch(&mockClientA, nullptr, 32, 500);
    QVERIFY(bridge.hasActiveDragFiles());
    QVERIFY(bridge.canUnlockEdgeSwitchForDrag(nullptr));
    QVERIFY(serverState->hoverVisible);
    QCOMPARE(serverState->hoverFileCount, 1u);
    QCOMPARE(serverState->hoverCursorPos, QPoint(32, 500));

    const auto *grantCmd = serverState->findCommand(deskflow::DropAgentCommandType::ReceiverAuthCommand);
    QVERIFY(grantCmd != nullptr);
    QVERIFY(grantCmd->text.rfind("GRANT ", 0) == 0);
    const std::string primaryToken = grantCmd->text.substr(6);
    QVERIFY(!primaryToken.empty());
    serverState->clearCommands();

    // --- Phase 3: Client A handles DESKSEND_COMMIT_HANDOFF -> Esc cancel -> LeftMouseUp, keeps pending files ---
    bridge.setDropExecutorForTesting(std::make_unique<RecordingDropExecutor>(clientState));
    bridge.setClientDragMessageSender([&](uint32_t count, const std::string &info) {
      clientToServerMsgs.emplace_back(count, info);
    });
    bridge.onClientReceiveDragInfo(0, mockClientA.dragMessages[0].second);
    QVERIFY(clientState->findCommand(deskflow::DropAgentCommandType::CancelNativeDrag) != nullptr);
    clientState->host->onDragCancellationCompleted(false);
    const auto *releaseCmd = clientState->findCommand(deskflow::DropAgentCommandType::LeftMouseUp);
    QVERIFY(releaseCmd != nullptr);
    QVERIFY(releaseCmd->flag);
    bridge.onClientLeave();
    clientState->clearCommands();

    // --- Phase 4: Cursor moves & drops on Primary ---
    bridge.setClientDragMessageSender({});
    bridge.setDropExecutorForTesting(std::make_unique<RecordingDropExecutor>(serverState));
    bridge.onPrimaryMouseMove(400, 300, 0, 0, 1920, 1080, 0);
    QCOMPARE(serverState->hoverCursorPos, QPoint(400, 300));

    bridge.onServerLeftMouseUp(nullptr, 400, 300);
    QVERIFY(!serverState->hoverVisible);
    QCOMPARE(serverState->receiverPhase, deskflow::TransferStatusOverlay::Phase::Preparing);
    QCOMPARE(serverState->preparingFileCount, 1u);
    const auto *dropTargetCmd = serverState->findCommand(deskflow::DropAgentCommandType::DropTarget);
    QVERIFY(dropTargetCmd != nullptr);
    QCOMPARE(dropTargetCmd->x, 400);
    QCOMPARE(dropTargetCmd->y, 300);
    QCOMPARE(dropTargetCmd->text, deskflow::FileDropBridge::computeTokenTag(primaryToken));

    QCOMPARE(mockClientA.dragMessages.size(), size_t{2});
    int32_t dropX = 0;
    int32_t dropY = 0;
    std::string wireTok;
    std::string wireTarget;
    QVERIFY(deskflow::parseSendDropMessage(mockClientA.dragMessages[1].second, dropX, dropY, wireTok, wireTarget));
    QCOMPARE(dropX, 400);
    QCOMPARE(dropY, 300);
    QCOMPARE(wireTok, primaryToken);
    QCOMPARE(wireTarget, std::string(deskflow::kServerPeerTargetMarker));

    // --- Phase 5: Client A executes DESKSEND_SEND_DROP -> sends to server IP ---
    bridge.setDropExecutorForTesting(std::make_unique<RecordingDropExecutor>(clientState));
    bridge.onClientReceiveDragInfo(1, mockClientA.dragMessages[1].second);
    const auto *sendCmd = clientState->findCommand(deskflow::DropAgentCommandType::SendFiles);
    QVERIFY(sendCmd != nullptr);
    QCOMPARE(sendCmd->text, std::string("10.0.0.1"));
    QCOMPARE(sendCmd->authToken, primaryToken);
    QCOMPARE(sendCmd->items, (std::vector<std::string>{samplePath.toStdString()}));
  }

  void clientAToClientB_viaPrimaryMultiHop_preservesSourceFilesAndCleansPrimaryOverlay()
  {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString samplePath = QDir(tempDir.path()).filePath(QStringLiteral("multi.txt"));
    {
      QFile f(samplePath);
      QVERIFY(f.open(QIODevice::WriteOnly));
      f.write("multihop");
    }

    auto &bridge = deskflow::FileDropBridge::instance();
    struct ResetGuard
    {
      deskflow::FileDropBridge &b;
      ~ResetGuard()
      {
        b.resetAllStateForTesting();
        b.setDropExecutorForTesting(nullptr);
      }
    } guard{bridge};
    bridge.resetAllStateForTesting();

    auto clientState = std::make_shared<RecordedExecutorState>();
    auto serverState = std::make_shared<RecordedExecutorState>();

    // Client A captures
    std::vector<std::pair<uint32_t, std::string>> clientToServerMsgs;
    bridge.setDropExecutorForTesting(std::make_unique<RecordingDropExecutor>(clientState));
    bridge.startClientReceiver("10.0.0.1");
    bridge.setClientDragMessageSender([&](uint32_t count, const std::string &info) {
      clientToServerMsgs.emplace_back(count, info);
    });
    bridge.onClientMouseLeftDown();
    clientState->host->onPendingDragCaptured({samplePath.toStdString()}, 1, 1919, 400);
    bridge.onClientReceiveDragInfo(0, std::string(deskflow::kMsgDragCommitHandoff));
    clientState->host->onDragCancellationCompleted(false);
    bridge.onClientLeave();
    clientState->clearCommands();

    // Server hops: Client A -> Primary -> Client B
    MockDragClientProxy clientA("clientA", "10.0.0.11");
    MockDragClientProxy clientB("clientB", "10.0.0.22");
    bridge.setClientDragMessageSender({});
    bridge.setDropExecutorForTesting(std::make_unique<RecordingDropExecutor>(serverState));
    bridge.onServerReceiveClientDragInfo(&clientA, clientToServerMsgs[0].first, clientToServerMsgs[0].second);
    bridge.prepareScreenSwitch(&clientA, nullptr);
    bridge.onScreenSwitch(&clientA, nullptr, 32, 400);
    QVERIFY(serverState->hoverVisible);
    const auto *primaryGrant = serverState->findCommand(deskflow::DropAgentCommandType::ReceiverAuthCommand);
    QVERIFY(primaryGrant != nullptr);
    const std::string tokPrimary = primaryGrant->text.substr(6);
    serverState->clearCommands();

    // Hop onto Client B -> Primary overlay & token MUST be cleaned up, Client B gets START + HOVER_ENTER
    bridge.onScreenSwitch(nullptr, &clientB, 32, 420);
    QVERIFY(!serverState->hoverVisible);
    QVERIFY(serverState->findCommand(deskflow::DropAgentCommandType::HideHover) != nullptr);
    const auto *cancelPrimaryTok = serverState->findCommand(deskflow::DropAgentCommandType::CancelDropToken);
    QVERIFY(cancelPrimaryTok != nullptr);
    QCOMPARE(cancelPrimaryTok->authToken, tokPrimary);

    QCOMPARE(clientB.dragMessages.size(), size_t{2});
    std::string tokB;
    QVERIFY(deskflow::parseDragStartMessage(clientB.dragMessages[0].second, tokB));
    QVERIFY(!tokB.empty());
    QVERIFY(tokB != tokPrimary);
    int32_t hx = 0;
    int32_t hy = 0;
    QVERIFY(deskflow::parseHoverEnterMessage(clientB.dragMessages[1].second, hx, hy));
    QCOMPARE(hx, 32);
    QCOMPARE(hy, 420);

    // Drop on Client B
    bridge.onServerLeftMouseUp(&clientB, 640, 480);
    QCOMPARE(clientB.dragMessages.size(), size_t{3});
    int32_t bx = 0;
    int32_t by = 0;
    std::string bTag;
    QVERIFY(deskflow::parseDragDropMessage(clientB.dragMessages[2].second, bx, by, bTag));
    QCOMPARE(bx, 640);
    QCOMPARE(by, 480);
    QCOMPARE(bTag, deskflow::FileDropBridge::computeTokenTag(tokB));

    QCOMPARE(clientA.dragMessages.size(), size_t{2});
    int32_t ax = 0;
    int32_t ay = 0;
    std::string aTok;
    std::string aTarget;
    QVERIFY(deskflow::parseSendDropMessage(clientA.dragMessages[1].second, ax, ay, aTok, aTarget));
    QCOMPARE(aTok, tokB);
    QCOMPARE(aTarget, std::string("10.0.0.22"));

    // Client A executes SendFiles directly to Client B's IP
    bridge.setDropExecutorForTesting(std::make_unique<RecordingDropExecutor>(clientState));
    bridge.onClientReceiveDragInfo(1, clientA.dragMessages[1].second);
    const auto *sendCmd = clientState->findCommand(deskflow::DropAgentCommandType::SendFiles);
    QVERIFY(sendCmd != nullptr);
    QCOMPARE(sendCmd->text, std::string("10.0.0.22"));
    QCOMPARE(sendCmd->authToken, tokB);
    QCOMPARE(sendCmd->items, (std::vector<std::string>{samplePath.toStdString()}));
  }

  void clientToPrimaryThenReturnToOrigin_cancelsCleanlyWithoutDrop()
  {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString samplePath = QDir(tempDir.path()).filePath(QStringLiteral("return.txt"));
    {
      QFile f(samplePath);
      QVERIFY(f.open(QIODevice::WriteOnly));
      f.write("return");
    }

    auto &bridge = deskflow::FileDropBridge::instance();
    struct ResetGuard
    {
      deskflow::FileDropBridge &b;
      ~ResetGuard()
      {
        b.resetAllStateForTesting();
        b.setDropExecutorForTesting(nullptr);
      }
    } guard{bridge};
    bridge.resetAllStateForTesting();

    auto clientState = std::make_shared<RecordedExecutorState>();
    auto serverState = std::make_shared<RecordedExecutorState>();

    std::vector<std::pair<uint32_t, std::string>> clientToServerMsgs;
    bridge.setDropExecutorForTesting(std::make_unique<RecordingDropExecutor>(clientState));
    bridge.setClientDragMessageSender([&](uint32_t count, const std::string &info) {
      clientToServerMsgs.emplace_back(count, info);
    });
    bridge.onClientMouseLeftDown();
    clientState->host->onPendingDragCaptured({samplePath.toStdString()}, 1, 1919, 500);
    bridge.onClientReceiveDragInfo(0, std::string(deskflow::kMsgDragCommitHandoff));
    clientState->host->onDragCancellationCompleted(false);
    clientState->clearCommands();

    MockDragClientProxy clientA("clientA", "10.0.0.11");
    bridge.setClientDragMessageSender({});
    bridge.setDropExecutorForTesting(std::make_unique<RecordingDropExecutor>(serverState));
    bridge.onServerReceiveClientDragInfo(&clientA, clientToServerMsgs[0].first, clientToServerMsgs[0].second);
    bridge.prepareScreenSwitch(&clientA, nullptr);
    bridge.onScreenSwitch(&clientA, nullptr, 32, 500);
    QVERIFY(serverState->hoverVisible);
    serverState->clearCommands();

    // Return back to origin Client A -> cancels hover/token on Primary & sends DESKSEND_SEND_CANCEL to Client A
    bridge.onScreenSwitch(nullptr, &clientA, 1887, 500);
    QVERIFY(!serverState->hoverVisible);
    QVERIFY(serverState->findCommand(deskflow::DropAgentCommandType::HideHover) != nullptr);
    QVERIFY(serverState->findCommand(deskflow::DropAgentCommandType::CancelDropToken) != nullptr);
    QVERIFY(!bridge.hasActiveDragFiles());
    QVERIFY(!bridge.canUnlockEdgeSwitchForDrag(nullptr));
    QCOMPARE(clientA.dragMessages.size(), size_t{2});
    std::string cancelTok;
    QVERIFY(deskflow::parseSendCancelMessage(clientA.dragMessages[1].second, cancelTok));

    // Client A processes SEND_CANCEL -> clears pending files so no SendFiles can happen
    bridge.setDropExecutorForTesting(std::make_unique<RecordingDropExecutor>(clientState));
    bridge.onClientReceiveDragInfo(0, clientA.dragMessages[1].second);
    bridge.onClientReceiveDragInfo(1, deskflow::serializeSendDropMessage(100, 100, "lateTok", "__SERVER__"));
    QCOMPARE(clientState->countCommands(deskflow::DropAgentCommandType::SendFiles), size_t{0});
  }

  void clientDisconnectDuringMultiHop_resetsLockAndNotifiesSurvivingEndpoint()
  {
    auto &bridge = deskflow::FileDropBridge::instance();
    struct ResetGuard
    {
      deskflow::FileDropBridge &b;
      ~ResetGuard()
      {
        b.resetAllStateForTesting();
        b.setDropExecutorForTesting(nullptr);
      }
    } guard{bridge};
    bridge.resetAllStateForTesting();

    auto serverState = std::make_shared<RecordedExecutorState>();
    bridge.setDropExecutorForTesting(std::make_unique<RecordingDropExecutor>(serverState));

    // Case A: Client A -> Primary -> Client B, then Client B disconnects -> notify Client A
    MockDragClientProxy clientA("clientA", "10.0.0.11");
    MockDragClientProxy clientB("clientB", "10.0.0.22");
    bridge.onServerReceiveClientDragInfo(&clientA, 1, "DESKSEND_CLIENT_PRECAPTURE:R:1919,500");
    bridge.prepareScreenSwitch(&clientA, nullptr);
    bridge.onScreenSwitch(&clientA, nullptr, 32, 500);
    bridge.onScreenSwitch(nullptr, &clientB, 32, 500);
    QVERIFY(bridge.hasActiveDragFiles());

    bridge.onClientDisconnected(&clientB);
    QVERIFY(!bridge.hasActiveDragFiles());
    QVERIFY(!bridge.canUnlockEdgeSwitchForDrag(nullptr));
    std::string sendCancelTok;
    QVERIFY(deskflow::parseSendCancelMessage(clientA.dragMessages.back().second, sendCancelTok));

    // Case B: Client A -> Primary, then Client A disconnects -> clean up Primary overlay/token
    bridge.resetAllStateForTesting();
    serverState->clearCommands();
    MockDragClientProxy clientA2("clientA", "10.0.0.11");
    bridge.onServerReceiveClientDragInfo(&clientA2, 2, "DESKSEND_CLIENT_PRECAPTURE:R:1919,500");
    bridge.prepareScreenSwitch(&clientA2, nullptr);
    bridge.onScreenSwitch(&clientA2, nullptr, 32, 500);
    QVERIFY(serverState->hoverVisible);
    serverState->clearCommands();

    bridge.onClientDisconnected(&clientA2);
    QVERIFY(!serverState->hoverVisible);
    QVERIFY(!bridge.hasActiveDragFiles());
    QVERIFY(!bridge.canUnlockEdgeSwitchForDrag(nullptr));
    QVERIFY(serverState->findCommand(deskflow::DropAgentCommandType::HideHover) != nullptr);
    QVERIFY(serverState->findCommand(deskflow::DropAgentCommandType::CancelDropToken) != nullptr);
  }

  void primaryToClient_preservedBackwardCompatibility()
  {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString samplePath = QDir(tempDir.path()).filePath(QStringLiteral("primary.txt"));
    {
      QFile f(samplePath);
      QVERIFY(f.open(QIODevice::WriteOnly));
      f.write("primary-to-client");
    }

    auto &bridge = deskflow::FileDropBridge::instance();
    struct ResetGuard
    {
      deskflow::FileDropBridge &b;
      ~ResetGuard()
      {
        b.resetAllStateForTesting();
        b.setDropExecutorForTesting(nullptr);
      }
    } guard{bridge};
    bridge.resetAllStateForTesting();

    auto serverState = std::make_shared<RecordedExecutorState>();
    bridge.setDropExecutorForTesting(std::make_unique<RecordingDropExecutor>(serverState));

    MockDragClientProxy clientB("clientB", "10.0.0.22");
    MockDragClientProxy clientC("clientC", "10.0.0.33");

    bridge.onPrimaryLeftMouseDown();
    QVERIFY(bridge.setPendingDragFiles({samplePath.toStdString()}));
    QVERIFY(bridge.canUnlockEdgeSwitchForDrag(nullptr));

    bridge.prepareScreenSwitch(nullptr, &clientB);
    bridge.onScreenSwitch(nullptr, &clientB, 32, 400);
    QVERIFY(bridge.hasActiveDragFiles());
    QCOMPARE(clientB.dragMessages.size(), size_t{2});
    std::string tokB;
    QVERIFY(deskflow::parseDragStartMessage(clientB.dragMessages[0].second, tokB));
    int32_t hx = 0;
    int32_t hy = 0;
    QVERIFY(deskflow::parseHoverEnterMessage(clientB.dragMessages[1].second, hx, hy));

    // Multi-hop Primary -> Client B -> Client C also works!
    bridge.onScreenSwitch(&clientB, &clientC, 32, 410);
    std::string cancelTokB;
    QVERIFY(deskflow::parseDragCancelMessage(clientB.dragMessages[2].second, cancelTokB));
    QCOMPARE(cancelTokB, tokB);

    QCOMPARE(clientC.dragMessages.size(), size_t{2});
    std::string tokC;
    QVERIFY(deskflow::parseDragStartMessage(clientC.dragMessages[0].second, tokC));

    serverState->clearCommands();
    bridge.onServerLeftMouseUp(&clientC, 700, 350);
    QCOMPARE(clientC.dragMessages.size(), size_t{3});
    int32_t cx = 0;
    int32_t cy = 0;
    std::string cTag;
    QVERIFY(deskflow::parseDragDropMessage(clientC.dragMessages[2].second, cx, cy, cTag));
    QCOMPARE(cTag, deskflow::FileDropBridge::computeTokenTag(tokC));

    const auto *sendCmd = serverState->findCommand(deskflow::DropAgentCommandType::SendFiles);
    QVERIFY(sendCmd != nullptr);
    QCOMPARE(sendCmd->text, std::string("10.0.0.33"));
    QCOMPARE(sendCmd->authToken, tokC);
    const auto *releaseCmd = serverState->findCommand(deskflow::DropAgentCommandType::LeftMouseUp);
    QVERIFY(releaseCmd != nullptr);
    QVERIFY(releaseCmd->flag);
  }

  void receiverAuthCache_tracksUnexpiredNonRevokedGrants()
  {
    deskflow::ReceiverAuthCache cache;
    cache.record("GRANT tok_a", 1000);
    cache.record("GRANT tok_b", 2000);
    cache.record("REVOKE tok_a", 3000);
    cache.record("IGNORE_UNKNOWN tok_c", 3500);

    auto active = cache.unexpiredGrants(4000);
    QCOMPARE(active.size(), size_t{1});
    QCOMPARE(active[0], std::string("GRANT tok_b"));

    // Grant expires after 300000 ms TTL
    QVERIFY(cache.unexpiredGrants(2000 + 300001).empty());

    // Capacity bounded at kMaxEntries (64) keeping newest grants
    cache.clear();
    for (size_t i = 0; i < 70; ++i) {
      cache.record("GRANT tok_" + std::to_string(i), 10000 + i);
    }
    const auto bounded = cache.unexpiredGrants(11000);
    QCOMPARE(bounded.size(), deskflow::ReceiverAuthCache::kMaxEntries);
    QCOMPARE(bounded.front(), std::string("GRANT tok_6"));
    QCOMPARE(bounded.back(), std::string("GRANT tok_69"));
  }

  void inProcessDropExecutor_replaysCachedGrantsOnReceiverRestart()
  {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    struct ScopedTestEnv
    {
      ScopedTestEnv(const QString &exe, const QString &dir)
      {
        qputenv("DESKSEND_LOCALSEND_CLI", exe.toUtf8());
        qputenv("DESKSEND_TEST_MOCK_CLI", "1");
        qputenv("DESKSEND_TEST_MOCK_CLI_MODE", "receiver_replay");
        qputenv("DESKSEND_TEST_MOCK_CLI_DIR", dir.toUtf8());
        qputenv("DESKSEND_RECEIVER_BACKOFF_MS", "20");
      }
      ~ScopedTestEnv()
      {
        qunsetenv("DESKSEND_LOCALSEND_CLI");
        qunsetenv("DESKSEND_TEST_MOCK_CLI");
        qunsetenv("DESKSEND_TEST_MOCK_CLI_MODE");
        qunsetenv("DESKSEND_TEST_MOCK_CLI_DIR");
        qunsetenv("DESKSEND_RECEIVER_BACKOFF_MS");
      }
    } envGuard(QCoreApplication::applicationFilePath(), tempDir.path());

    deskflow::InProcessDropExecutor executor(false);
    deskflow::DropAgentCommand startCmd;
    startCmd.type = deskflow::DropAgentCommandType::StartReceiver;
    startCmd.items = {"10.0.0.2"};
    executor.execute(startCmd);

    auto sendAuth = [&executor](const std::string &cmd) {
      deskflow::DropAgentCommand authCmd;
      authCmd.type = deskflow::DropAgentCommandType::ReceiverAuthCommand;
      authCmd.text = cmd;
      executor.execute(authCmd);
    };
    sendAuth("GRANT tok_revoked");
    sendAuth("REVOKE tok_revoked");
    sendAuth("GRANT tok_keep");

    const QString rx2ReadyFlag = tempDir.filePath(QStringLiteral("rx_2_ready.flag"));
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(rx2ReadyFlag), 5000);

    QFile rx2Log(tempDir.filePath(QStringLiteral("rx_2.log")));
    QVERIFY(rx2Log.open(QIODevice::ReadOnly));
    const QString contents = QString::fromUtf8(rx2Log.readAll());
    QVERIFY(contents.contains(QStringLiteral("GRANT tok_keep")));
    QVERIFY(!contents.contains(QStringLiteral("tok_revoked")));

    executor.execute({deskflow::DropAgentCommandType::StopReceiver});
  }

  void inProcessDropExecutor_capsConsecutiveEarlyReceiverFailures()
  {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    struct ScopedTestEnv
    {
      ScopedTestEnv(const QString &exe, const QString &dir)
      {
        qputenv("DESKSEND_LOCALSEND_CLI", exe.toUtf8());
        qputenv("DESKSEND_TEST_MOCK_CLI", "1");
        qputenv("DESKSEND_TEST_MOCK_CLI_MODE", "receiver_fail_early");
        qputenv("DESKSEND_TEST_MOCK_CLI_DIR", dir.toUtf8());
        qputenv("DESKSEND_RECEIVER_BACKOFF_MS", "15");
      }
      ~ScopedTestEnv()
      {
        qunsetenv("DESKSEND_LOCALSEND_CLI");
        qunsetenv("DESKSEND_TEST_MOCK_CLI");
        qunsetenv("DESKSEND_TEST_MOCK_CLI_MODE");
        qunsetenv("DESKSEND_TEST_MOCK_CLI_DIR");
        qunsetenv("DESKSEND_RECEIVER_BACKOFF_MS");
      }
    } envGuard(QCoreApplication::applicationFilePath(), tempDir.path());

    deskflow::InProcessDropExecutor executor(false);
    deskflow::DropAgentCommand startCmd;
    startCmd.type = deskflow::DropAgentCommandType::StartReceiver;
    startCmd.items = {"10.0.0.2"};
    executor.execute(startCmd);

    const QString countPath = tempDir.filePath(QStringLiteral("fail_count.txt"));
    auto launchCount = [&countPath]() -> int {
      QFile f(countPath);
      if (!f.open(QIODevice::ReadOnly)) {
        return 0;
      }
      return static_cast<int>(f.readAll().size());
    };

    const int expectedLaunches = 1 + deskflow::InProcessDropExecutor::kMaxConsecutiveReceiverRestarts;
    QTRY_COMPARE_WITH_TIMEOUT(launchCount(), expectedLaunches, 5000);
    QTest::qWait(150);
    QCOMPARE(launchCount(), expectedLaunches);
    QVERIFY(!executor.m_receiverDesired);
    QCOMPARE(executor.overlayFor(true).phase(), deskflow::TransferStatusOverlay::Phase::Finished);
  }

  void inProcessDropExecutor_sendWatchdogKillsStalledJobAndAdvancesQueue()
  {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString samplePath = tempDir.filePath(QStringLiteral("sample.txt"));
    {
      QFile f(samplePath);
      QVERIFY(f.open(QIODevice::WriteOnly));
      f.write("hello");
    }

    struct ScopedTestEnv
    {
      ScopedTestEnv(const QString &exe, const QString &dir)
      {
        qputenv("DESKSEND_LOCALSEND_CLI", exe.toUtf8());
        qputenv("DESKSEND_TEST_MOCK_CLI", "1");
        qputenv("DESKSEND_TEST_MOCK_CLI_MODE", "send_watchdog");
        qputenv("DESKSEND_TEST_MOCK_CLI_DIR", dir.toUtf8());
        qputenv("DESKSEND_SEND_INACTIVITY_TIMEOUT_MS", "150");
      }
      ~ScopedTestEnv()
      {
        qunsetenv("DESKSEND_LOCALSEND_CLI");
        qunsetenv("DESKSEND_TEST_MOCK_CLI");
        qunsetenv("DESKSEND_TEST_MOCK_CLI_MODE");
        qunsetenv("DESKSEND_TEST_MOCK_CLI_DIR");
        qunsetenv("DESKSEND_SEND_INACTIVITY_TIMEOUT_MS");
      }
    } envGuard(QCoreApplication::applicationFilePath(), tempDir.path());

    struct SendHost final : public deskflow::IDropExecutorHost
    {
      void onPendingDragCaptured(std::vector<std::string>, int, int32_t, int32_t) override {}
      void onDragCancellationCompleted(bool) override {}
      void onPendingDragCleared() override {}
      void onActiveTransfersChanged(bool) override {}
      void onSendCompleted(const std::string &targetIp, bool success) override
      {
        results.emplace_back(targetIp, success);
      }
      std::vector<std::pair<std::string, bool>> results;
    } host;

    deskflow::InProcessDropExecutor executor(false);
    executor.setHost(&host);

    deskflow::DropAgentCommand job1;
    job1.type = deskflow::DropAgentCommandType::SendFiles;
    job1.text = "10.0.0.10";
    job1.items = {samplePath.toStdString()};
    job1.authToken = "tok1";
    executor.execute(job1);

    deskflow::DropAgentCommand job2;
    job2.type = deskflow::DropAgentCommandType::SendFiles;
    job2.text = "10.0.0.11";
    job2.items = {samplePath.toStdString()};
    job2.authToken = "tok2";
    executor.execute(job2);

    QTRY_COMPARE_WITH_TIMEOUT(host.results.size(), size_t{2}, 5000);
    QCOMPARE(host.results[0].first, std::string("10.0.0.10"));
    QCOMPARE(host.results[0].second, false);
    QCOMPARE(host.results[1].first, std::string("10.0.0.11"));
    QCOMPARE(host.results[1].second, true);
    QVERIFY(!executor.hasActiveTransfers());
  }
};

namespace {

int runMockLocalSendCli()
{
  const QString mode = qEnvironmentVariable("DESKSEND_TEST_MOCK_CLI_MODE");
  const QString dirPath = qEnvironmentVariable("DESKSEND_TEST_MOCK_CLI_DIR");
  QDir dir(dirPath);

  if (mode == QLatin1String("receiver_fail_early")) {
    QFile f(dir.filePath(QStringLiteral("fail_count.txt")));
    if (f.open(QIODevice::Append | QIODevice::WriteOnly)) {
      f.write("x");
      f.flush();
    }
    return 1;
  }

  if (mode == QLatin1String("receiver_replay")) {
    const QString marker = dir.filePath(QStringLiteral("launch_1.done"));
    const bool isFirst = !QFileInfo::exists(marker);
    if (isFirst) {
      QFile logFile(dir.filePath(QStringLiteral("rx_1.log")));
      std::string line;
      while (std::getline(std::cin, line)) {
        if (logFile.open(QIODevice::Append | QIODevice::WriteOnly)) {
          logFile.write(line.c_str(), static_cast<qint64>(line.size()));
          logFile.write("\n");
          logFile.close();
        }
        if (line == "GRANT tok_keep") {
          break;
        }
      }
      QFile doneFile(marker);
      if (doneFile.open(QIODevice::WriteOnly)) {
        doneFile.close();
      }
      return 1;
    }

    QFile logFile(dir.filePath(QStringLiteral("rx_2.log")));
    std::string line;
    while (std::getline(std::cin, line)) {
      if (logFile.open(QIODevice::Append | QIODevice::WriteOnly)) {
        logFile.write(line.c_str(), static_cast<qint64>(line.size()));
        logFile.write("\n");
        logFile.close();
      }
      if (line == "GRANT tok_keep") {
        std::cout << "LOCALSEND_RECEIVER_READY:53318\n" << std::flush;
        QFile readyFile(dir.filePath(QStringLiteral("rx_2_ready.flag")));
        if (readyFile.open(QIODevice::WriteOnly)) {
          readyFile.close();
        }
      }
    }
    return 0;
  }

  if (mode == QLatin1String("send_watchdog")) {
    const QString marker = dir.filePath(QStringLiteral("send_1.done"));
    const bool isFirst = !QFileInfo::exists(marker);
    if (isFirst) {
      QFile doneFile(marker);
      if (doneFile.open(QIODevice::WriteOnly)) {
        doneFile.close();
      }
      std::this_thread::sleep_for(std::chrono::seconds(5));
      return 1;
    }
    std::cout << "SEND_PROGRESS:5:5\n" << std::flush;
    return 0;
  }

  return 1;
}

} // namespace

int main(int argc, char *argv[])
{
  QCoreApplication app(argc, argv);
  if (qEnvironmentVariableIsSet("DESKSEND_TEST_MOCK_CLI")) {
    return runMockLocalSendCli();
  }
  QStringList args;
  args.reserve(argc + 3);
  bool hasOutputOpt = false;
  for (int i = 0; i < argc; ++i) {
    const QString arg = QString::fromLocal8Bit(argv[i]);
    if (arg == QLatin1String("-o")) {
      hasOutputOpt = true;
    }
    args.append(arg);
  }
  QString diagPath;
  if (!hasOutputOpt) {
    diagPath = QDir::temp().filePath(
        QStringLiteral("FileDropBridgeTests_qtest_%1.txt").arg(QCoreApplication::applicationPid())
    );
    args << QStringLiteral("-o") << (diagPath + QStringLiteral(",txt"));
  }
  FileDropBridgeTests tc;
  const int rc = QTest::qExec(&tc, args);
  if (!diagPath.isEmpty()) {
    if (rc != 0) {
      QFile f(diagPath);
      if (f.open(QIODevice::ReadOnly)) {
        std::cerr << f.readAll().toStdString() << std::flush;
        f.close();
      }
      std::cerr << "FileDropBridgeTests QTest::qExec rc=" << rc << std::endl;
    }
    QFile::remove(diagPath);
  }
  return rc;
}
#include "FileDropBridgeTests.moc"
