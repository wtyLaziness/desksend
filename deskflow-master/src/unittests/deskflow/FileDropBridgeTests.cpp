// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "deskflow/DragOverlayWidgets.h"
#include "deskflow/DragProtocol.h"
#include "deskflow/DropAgentIpc.h"
#include "deskflow/DropExecutor.h"
#include "deskflow/FileDropBridge.h"
#include "base/Log.h"
#include "server/BaseClientProxy.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <chrono>

namespace {

class MockDragClientProxy final : public BaseClientProxy
{
public:
  explicit MockDragClientProxy(const std::string &name) : BaseClientProxy(name)
  {
  }

  std::vector<std::pair<uint32_t, std::string>> dragMessages;

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
  deskflow::IStream *getStream() const override { return nullptr; }
};

} // namespace

class FileDropBridgeTests : public QObject
{
  Q_OBJECT
private:
  Log m_log;

private Q_SLOTS:
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
        deskflow::clampDragHoverPosition(QPoint(-1925, -10), capsuleSize, QRect(-1920, 0, 1920, 1080));
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

    // Also test full Preparing -> Active -> Finished progression
    bridge.onClientReceiveDragInfo(1, "DESKSEND_START:tokHover3");
    const QString tag3 = QString::fromStdString(deskflow::FileDropBridge::computeTokenTag("tokHover3"));
    bridge.onClientReceiveDragInfo(1, "DESKSEND_DROP:300,400:" + tag3.toStdString());
    QCOMPARE(bridge.receiverOverlayPhase(), deskflow::TransferStatusOverlay::Phase::Preparing);
    bridge.handleReceiveSessionStarted(1, tag3.toStdString());
    QCOMPARE(bridge.receiverOverlayPhase(), deskflow::TransferStatusOverlay::Phase::Active);
    bridge.handleReceiveSessionFinished(false);
    QCOMPARE(bridge.receiverOverlayPhase(), deskflow::TransferStatusOverlay::Phase::Finished);

    qunsetenv("DESKSEND_DEFAULT_DROP_DIR");
  }

  void dropAgentIpc_roundTripsAllCommandsAndEvents()
  {
    using namespace deskflow;

    const std::vector<DropAgentCommand> commands = {
        {DropAgentCommandType::UpdatePrimaryShape, 10, 20, 0, 0, 1920, 1080},
        {DropAgentCommandType::PrimaryMouseMove, 500, 600, 0, 0, 1920, 1080, 0x06,
         {{DragEdgeSide::Right, 16, 1064}}},
        {DropAgentCommandType::PrimaryLeftMouseDown},
        [] {
          DropAgentCommand c;
          c.type = DropAgentCommandType::LeftMouseUp;
          c.flag = true;
          return c;
        }(),
        {DropAgentCommandType::HideSentinels},
        {DropAgentCommandType::ShutdownSentinels},
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
        {DropAgentCommandType::UpdateHoverCursor, 300, 400},
        {DropAgentCommandType::HideHover},
        [] {
          DropAgentCommand c;
          c.type = DropAgentCommandType::BeginPreparing;
          c.fileCount = 3;
          return c;
        }(),
        {DropAgentCommandType::CancelPreparing},
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
          c.text = "Done";
          return c;
        }(),
        [] {
          DropAgentCommand c;
          c.type = DropAgentCommandType::StartReceiver;
          c.items = {"192.168.1.10", "2001:db8::5"};
          return c;
        }(),
        {DropAgentCommandType::StopReceiver},
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
        {DropAgentCommandType::Shutdown},
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
};

QTEST_MAIN(FileDropBridgeTests)
#include "FileDropBridgeTests.moc"
