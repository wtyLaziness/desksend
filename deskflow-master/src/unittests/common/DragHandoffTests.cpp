// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "deskflow/DragHandoff.h"
#include <QtTest>
class DragHandoffTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void reverseMotionAfterEntry()
  {
    const auto entry = deskflow::fileDragEntryCoordinate(0, 0, 1920);
    QCOMPARE(entry, 32);
    // 复现真机日志中的首次 -11 像素反向位移，仍留在副屏。
    QVERIFY(entry - 11 >= 0);
    // 用户明确移回边缘时仍可正常返回，不依赖按键释放或超时锁定。
    QVERIFY(entry - 64 < 0);
    QCOMPARE(deskflow::fileDragEntryCoordinate(1919, 0, 1920), 1887);
    QCOMPARE(deskflow::fileDragEntryCoordinate(-1920, -1920, 1920), -1888);
    QCOMPARE(deskflow::fileDragEntryCoordinate(613, 0, 1080), 613);
  }
  void smallScreen()
  {
    QCOMPARE(deskflow::fileDragEntryCoordinate(0, 0, 1), 0);
    QCOMPARE(deskflow::fileDragEntryCoordinate(0, 0, 2), 0);
    QCOMPARE(deskflow::fileDragEntryCoordinate(1, 0, 2), 1);
    QCOMPARE(deskflow::fileDragEntryCoordinate(0, 0, 9), 4);
  }
};
QTEST_MAIN(DragHandoffTests)
#include "DragHandoffTests.moc"
