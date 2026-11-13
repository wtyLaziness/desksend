// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
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

#ifdef Q_OS_WIN
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
