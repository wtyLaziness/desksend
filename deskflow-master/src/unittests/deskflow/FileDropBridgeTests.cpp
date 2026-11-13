// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "deskflow/FileDropBridge.h"
#include "base/Log.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

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
};

QTEST_MAIN(FileDropBridgeTests)
#include "FileDropBridgeTests.moc"
