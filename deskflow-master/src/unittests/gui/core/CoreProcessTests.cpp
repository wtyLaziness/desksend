// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "common/VersionInfo.h"
#include "gui/ipc/IpcClient.h"
#include <QLocalServer>
#include <QLocalSocket>
#include <QSignalSpy>
#include <QUuid>

#include "common/Settings.h"
#include "gui/config/ServerConfig.h"
#include "gui/core/CoreProcess.h"

#include <QApplication>
#include <QCoreApplication>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

#ifdef Q_OS_WIN
#include "arch/win32/ArchDaemonWindows.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

class CoreProcessTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase()
  {
    QVERIFY(m_tempDir.isValid());
    Settings::setSettingsFile(m_tempDir.filePath(QStringLiteral("Deskflow.conf")));
    Settings::setStateFile(m_tempDir.filePath(QStringLiteral("Deskflow.state")));
    Settings::setValue(Settings::Core::ProcessMode, Settings::ProcessMode::Desktop);
  }

  void cleanupStopsProcessEvenWhenAlreadyStopping()
  {
    ServerConfig serverConfig;
    deskflow::gui::CoreProcess coreProcess(serverConfig);

    auto *proc = new QProcess(&coreProcess);
    proc->start(QCoreApplication::applicationFilePath(), {QStringLiteral("--desksend-test-sleep-child")});
    QVERIFY(proc->waitForStarted(5000));

#ifdef Q_OS_WIN
    QVERIFY(coreProcess.attachToWinJobObject(proc->processId()));
#endif
    coreProcess.m_process = proc;
    coreProcess.m_processState = deskflow::core::ProcessState::Stopping;

    coreProcess.cleanup();

    QCOMPARE(proc->state(), QProcess::NotRunning);
    QCOMPARE(coreProcess.processState(), deskflow::core::ProcessState::Stopped);
  }

  void transferPeersFollowConnectionAndRejectMalformedPayload()
  {
    ServerConfig serverConfig;
    deskflow::gui::CoreProcess coreProcess(serverConfig);
    const auto update = [&coreProcess](const QByteArray &payload) {
      coreProcess.onCoreIpcMessageReceived(QStringLiteral("transferPeers"), QString::fromLatin1(payload));
    };
    coreProcess.setMode(Settings::CoreMode::Server);
    const QByteArray peers = QByteArray(R"(["__SERVER__","peer"])").toBase64();
    update(peers);
    QVERIFY(coreProcess.transferPeers().isEmpty());
    coreProcess.setConnectionState(deskflow::core::ConnectionState::Connected);
    update(peers);
    QCOMPARE(coreProcess.transferPeers().size(), 2);
    update(peers + "!");
    QVERIFY(coreProcess.transferPeers().isEmpty());
    update(peers);
    update(QByteArray(R"(["peer",42])").toBase64());
    QVERIFY(coreProcess.transferPeers().isEmpty());
    update(peers);
    coreProcess.setMode(Settings::CoreMode::Client);
    QVERIFY(coreProcess.transferPeers().isEmpty());
    update(peers);
    coreProcess.setConnectionState(deskflow::core::ConnectionState::Disconnected);
    update(peers);
    QVERIFY(coreProcess.transferPeers().isEmpty());
  }

  void sendFilesFailureIsForwarded()
  {
    ServerConfig serverConfig;
    deskflow::gui::CoreProcess coreProcess(serverConfig);
    QSignalSpy failure(&coreProcess, &deskflow::gui::CoreProcess::sendFilesFailed);
    coreProcess.onCoreIpcMessageReceived(QStringLiteral("error"), QStringLiteral("sendFilesFailed"));
    QCOMPARE(failure.count(), 1);
  }

  void activeTransfersUpdatedViaCoreIpc()
  {
    ServerConfig serverConfig;
    deskflow::gui::CoreProcess coreProcess(serverConfig);

    QVERIFY(!coreProcess.hasActiveFileTransfers());
    coreProcess.onCoreIpcMessageReceived(QStringLiteral("activeTransfers"), QStringLiteral("1"));
    QVERIFY(coreProcess.hasActiveFileTransfers());
    coreProcess.onCoreIpcMessageReceived(QStringLiteral("activeTransfers"), QStringLiteral("0"));
    QVERIFY(!coreProcess.hasActiveFileTransfers());
  }

#ifdef Q_OS_WIN
  void stoppingAbsentServiceSucceeds()
  {
    const auto name = QStringLiteral("DeskSendTest-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    ArchDaemonWindows::stop(name);
  }

  void installedServiceStopsOnExit()
  {
    if (qEnvironmentVariableIntValue("DESKSEND_TEST_MSI_SERVICE") != 1) {
      QSKIP("Requires the isolated MSI installation test.");
    }

    ServerConfig serverConfig;
    deskflow::gui::CoreProcess coreProcess(serverConfig);
    QVERIFY(coreProcess.stopServiceForExit());
    ArchDaemonWindows::start(QString::fromWCharArray(kWindowsServiceName));
    QVERIFY(coreProcess.stopServiceForExit());
  }

  void winJobObjectKillsChildWhenCoreProcessDestroyed()
  {
    ServerConfig serverConfig;
    qint64 pid = 0;
    QVERIFY(QProcess::startDetached(
        QCoreApplication::applicationFilePath(), {QStringLiteral("--desksend-test-sleep-child")}, QString(), &pid
    ));
    QVERIFY(pid > 0);

    HANDLE childHandle = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
    QVERIFY(childHandle != nullptr);

    {
      deskflow::gui::CoreProcess coreProcess(serverConfig);
      QVERIFY(coreProcess.attachToWinJobObject(pid));
      QCOMPARE(WaitForSingleObject(childHandle, 50), WAIT_TIMEOUT);
    }

    const DWORD waitResult = WaitForSingleObject(childHandle, 5000);
    if (waitResult != WAIT_OBJECT_0) {
      TerminateProcess(childHandle, 1);
    }
    CloseHandle(childHandle);
    QCOMPARE(waitResult, WAIT_OBJECT_0);
  }
#endif

  void ipcReconnectClearsPartialLine()
  {
    const auto name = QStringLiteral("ds-reconnect-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    QLocalServer server;
    server.setSocketOptions(QLocalServer::UserAccessOption);
    QVERIFY(server.listen(name));
    deskflow::gui::ipc::IpcClient client(nullptr, name, QStringLiteral("test"));
    QSignalSpy connected(&client, &deskflow::gui::ipc::IpcClient::connected);
    QSignalSpy failed(&client, &deskflow::gui::ipc::IpcClient::connectionFailed);
    const auto hello = QStringLiteral("hello=%1+%2\n").arg(kVersion, kVersionGitSha).toUtf8();

    client.connectToServer();
    QTRY_VERIFY(server.hasPendingConnections());
    auto *first = server.nextPendingConnection();
    QTRY_VERIFY(first->canReadLine());
    QCOMPARE(first->readLine(), hello);
    first->write("stale");
    first->flush();
    QTest::qWait(50);
    first->disconnectFromServer();
    QTRY_COMPARE(failed.size(), 1);

    client.connectToServer();
    QTRY_VERIFY(server.hasPendingConnections());
    auto *second = server.nextPendingConnection();
    QTRY_VERIFY(second->canReadLine());
    QCOMPARE(second->readLine(), hello);
    second->write(hello);
    second->flush();
    QTRY_COMPARE(connected.size(), 1);
  }

private:
  QTemporaryDir m_tempDir;
};

int main(int argc, char *argv[])
{
  if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--desksend-test-sleep-child")) {
#ifdef Q_OS_WIN
    Sleep(60000);
#else
    QThread::sleep(60);
#endif
    return 0;
  }

  QApplication app(argc, argv);
  CoreProcessTests tc;
  return QTest::qExec(&tc, argc, argv);
}

#include "CoreProcessTests.moc"
