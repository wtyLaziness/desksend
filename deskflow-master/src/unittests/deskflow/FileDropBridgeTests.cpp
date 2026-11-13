// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
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
    {
      std::scoped_lock lock(bridge.m_mutex);
      bridge.m_receiverDesired = true;
      bridge.m_defaultDropFallbackDir = fallback.path().toStdString();
    }
    bridge.onClientReceiveDragInfo(1, "DESKSEND_DROP:999999,999999:" + tagA);
    {
      std::scoped_lock lock(bridge.m_mutex);
      QCOMPARE(bridge.m_pendingDropTargets.size(), size_t{1});
      QCOMPARE(bridge.m_pendingDropTargets.back().tokenTag, tagA);
      bridge.m_pendingDropTargets.back().dir = dirA.path().toStdString();
      const uint64_t t = steadyNowMs();
      bridge.m_pendingDropTargets.push_back({dirB.path().toStdString(), true, t, tagB});
      bridge.m_pendingDropTargets.push_back({dirC.path().toStdString(), false, t, tagC});
    }

    // Cancel token A only: removes tag A while leaving tag B and tag C intact
    bridge.onClientReceiveDragInfo(0, "DESKSEND_CANCEL:" + tokenA);
    {
      std::scoped_lock lock(bridge.m_mutex);
      QCOMPARE(bridge.m_pendingDropTargets.size(), size_t{2});
      QCOMPARE(bridge.m_pendingDropTargets[0].tokenTag, tagB);
      QCOMPARE(bridge.m_pendingDropTargets[1].tokenTag, tagC);
      QVERIFY(!bridge.m_pendingReceiverAuthLines.empty());
      QCOMPARE(bridge.m_pendingReceiverAuthLines.back(), std::string("REVOKE " + tokenA));
      // Refresh timestamps relative to steady_clock so neither expires
      const uint64_t now = steadyNowMs();
      bridge.m_pendingDropTargets[0].timestampMs = now;
      bridge.m_pendingDropTargets[1].timestampMs = now;
    }

    // Session for token C arrives even though orphaned tag B never started: jumps to C and discards B
    bridge.handleReceiveSessionStarted(2, tagC);
    {
      std::scoped_lock lock(bridge.m_mutex);
      QCOMPARE(bridge.m_activeSessionTarget.dir, dirC.path().toStdString());
      QCOMPARE(bridge.m_activeSessionTarget.tokenTag, tagC);
      QVERIFY(!bridge.m_activeSessionTarget.copyToClipboard);
      QVERIFY(bridge.m_pendingDropTargets.empty());
    }

    bridge.stopReceiver();
  }
};

QTEST_MAIN(FileDropBridgeTests)
#include "FileDropBridgeTests.moc"
