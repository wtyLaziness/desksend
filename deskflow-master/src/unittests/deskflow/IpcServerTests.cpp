// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception

#include "base/Log.h"
#include "common/VersionInfo.h"
#include "deskflow/ipc/IpcServer.h"

#include <QLocalSocket>
#include <QSignalSpy>
#include <QTest>
#include <QUuid>

namespace {

class TestServer final : public deskflow::core::ipc::IpcServer
{
public:
  explicit TestServer(const QString &name) : IpcServer(nullptr, name, QStringLiteral("test"))
  {
  }

private:
  void processCommand(QLocalSocket *, const QString &command, const QStringList &parts) override
  {
    if (command == QStringLiteral("configFile")) {
      Q_EMIT configFileChanged(parts.value(1));
    } else if (command == QStringLiteral("start")) {
      Q_EMIT startProcessRequested();
    }
  }
};

} // namespace

class IpcServerTests : public QObject
{
  Q_OBJECT

private Q_SLOTS:
  void keepsPartialConfigCommandUntilFinalChunk()
  {
    const auto name = QStringLiteral("ds-ipc-%1").arg(QUuid::createUuid().toString(QUuid::Id128).left(12));
    TestServer server(name);
    server.listen();

    QSignalSpy configChanged(&server, &deskflow::core::ipc::IpcServer::configFileChanged);
    QSignalSpy startRequested(&server, &deskflow::core::ipc::IpcServer::startProcessRequested);
    QLocalSocket client;
    client.connectToServer(name);
    QVERIFY(client.waitForConnected());

    client.write(hello() + "noop\nconfigFile=C:/Desk");
    QTRY_VERIFY(client.canReadLine());
    QCOMPARE(client.readLine(), hello());
    QTRY_VERIFY(client.canReadLine());
    QCOMPARE(client.readLine(), QByteArray("ok\n"));
    QCOMPARE(configChanged.size(), 0);

    client.write("=Send/Deskflow.conf\nstart\n");
    QTRY_COMPARE(startRequested.size(), 1);
    QCOMPARE(configChanged.size(), 1);
    QCOMPARE(configChanged.takeFirst().at(0).toString(), QStringLiteral("C:/Desk=Send/Deskflow.conf"));
  }

  void rejectsCommandBeforeHello()
  {
    const auto name = QStringLiteral("ds-reject-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    TestServer endpoint(name);
    QSignalSpy commands(&endpoint, &deskflow::core::ipc::IpcServer::startProcessRequested);
    endpoint.listen();
    QLocalSocket client;
    client.connectToServer(name);
    QVERIFY(client.waitForConnected());
    client.write("start\n");
    QTRY_COMPARE(client.state(), QLocalSocket::UnconnectedState);
    QVERIFY(commands.isEmpty());
  }

  void boundsOfflineQueueAndRejectsOversizeLine()
  {
    const auto name = QStringLiteral("ds-limit-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    TestServer endpoint(name);
    endpoint.listen();
    for (int i = 0; i < 70; ++i)
      endpoint.broadcastCommand(QStringLiteral("queued"), QString::number(i));
    QLocalSocket client;
    client.connectToServer(name);
    QVERIFY(client.waitForConnected());
    client.write(hello());
    QTRY_VERIFY(client.canReadLine());
    QCOMPARE(client.readLine(), hello());
    for (int i = 6; i < 70; ++i) {
      QTRY_VERIFY(client.canReadLine());
      QCOMPARE(client.readLine(), "queued=" + QByteArray::number(i) + '\n');
    }
    client.write(QByteArray(128 * 1024, 'x'));
    QTRY_COMPARE(client.state(), QLocalSocket::UnconnectedState);
  }

private:
  static QByteArray hello()
  {
    return QStringLiteral("hello=%1+%2\n").arg(kVersion, kVersionGitSha).toUtf8();
  }

  Log m_log;
};

QTEST_GUILESS_MAIN(IpcServerTests)
#include "IpcServerTests.moc"
