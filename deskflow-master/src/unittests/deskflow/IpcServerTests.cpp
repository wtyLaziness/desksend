// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception

#include "base/Log.h"
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
    const auto name = QStringLiteral("desksend-test-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    TestServer server(name);
    server.listen();

    QSignalSpy configChanged(&server, &deskflow::core::ipc::IpcServer::configFileChanged);
    QSignalSpy startRequested(&server, &deskflow::core::ipc::IpcServer::startProcessRequested);
    QLocalSocket client;
    client.connectToServer(name);
    QVERIFY(client.waitForConnected());

    client.write("noop\nconfigFile=C:/Desk");
    QTRY_VERIFY(client.canReadLine());
    QCOMPARE(client.readLine(), QByteArray("ok\n"));
    QCOMPARE(configChanged.size(), 0);

    client.write("=Send/Deskflow.conf\nstart\n");
    QTRY_COMPARE(startRequested.size(), 1);
    QCOMPARE(configChanged.size(), 1);
    QCOMPARE(configChanged.takeFirst().at(0).toString(), QStringLiteral("C:/Desk=Send/Deskflow.conf"));
  }

private:
  Log m_log;
};

QTEST_GUILESS_MAIN(IpcServerTests)
#include "IpcServerTests.moc"
