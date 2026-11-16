/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "arch/Arch.h"
#include "base/EventQueue.h"
#include "base/IJob.h"
#include "base/Log.h"
#include "deskflow/IScreenSaver.h"
#include "deskflow/win32/AppUtilWindows.h"
#include "platform/MSWindowsComputer.h"
#include "platform/MSWindowsDesks.h"

#include <QTest>

#include <cstdint>
#include <memory>

namespace {

class DummyScreenSaver final : public IScreenSaver
{
public:
  void enable() override
  {
  }
  void disable() override
  {
  }
  void activate() override
  {
  }
  void deactivate() override
  {
  }
  bool isActive() const override
  {
    return m_active;
  }

  bool m_active = false;
};

class NoopJob final : public IJob
{
public:
  void run() override
  {
  }
};

} // namespace

class MSWindowsDesksTests : public QObject
{
  Q_OBJECT

private Q_SLOTS:
  void initTestCase()
  {
    m_arch.init();
    m_events = std::make_unique<EventQueue>();
    m_appUtil = std::make_unique<AppUtilWindows>(m_events.get());
    if (MSWindowsComputer::getWindowInstance() == nullptr) {
      MSWindowsComputer::init(GetModuleHandleW(nullptr));
    }
  }

  void cleanupTestCase()
  {
    m_appUtil.reset();
    m_events.reset();
  }

  void enterAndLeave_whenActiveDeskInaccessible_updatesLogicalState()
  {
    DummyScreenSaver screensaver;
    MSWindowsDesks::Desk inaccessibleDesk{};
    inaccessibleDesk.m_name = L"";
    inaccessibleDesk.m_thread = nullptr;
    inaccessibleDesk.m_threadID = 0;
    inaccessibleDesk.m_targetID = 0;
    inaccessibleDesk.m_desk = nullptr;
    inaccessibleDesk.m_window = nullptr;
    inaccessibleDesk.m_foregroundWindow = nullptr;
    inaccessibleDesk.m_lowLevel = false;

    MSWindowsDesks desks(false, false, &screensaver, m_events.get(), new NoopJob());
    QVERIFY(!desks.m_isOnComputer);
    QCOMPARE(desks.m_activeDesk, nullptr);

    // No active desk yet: logical state must still update.
    desks.enter();
    QVERIFY(desks.m_isOnComputer);

    const HKL layout = reinterpret_cast<HKL>(static_cast<uintptr_t>(0x04090409));
    desks.leave(layout);
    QVERIFY(!desks.m_isOnComputer);
    QCOMPARE(desks.m_keyLayout, layout);

    // Inaccessible desktop (m_window == nullptr, e.g. UAC Winlogon desktop).
    desks.m_activeDesk = &inaccessibleDesk;
    desks.enter();
    QVERIFY(desks.m_isOnComputer);

    desks.leave(layout);
    QVERIFY(!desks.m_isOnComputer);
    QCOMPARE(desks.m_keyLayout, layout);
    desks.m_activeDesk = nullptr;
  }

  void checkDesk_returningFromInaccessibleDesk_preservesLogicalStateAndShowsCursor()
  {
    DummyScreenSaver screensaver;
    MSWindowsDesks::Desk inaccessibleDesk{};
    inaccessibleDesk.m_name = L"";
    inaccessibleDesk.m_thread = nullptr;
    inaccessibleDesk.m_threadID = 0;
    inaccessibleDesk.m_targetID = 0;
    inaccessibleDesk.m_desk = nullptr;
    inaccessibleDesk.m_window = nullptr;
    inaccessibleDesk.m_foregroundWindow = nullptr;
    inaccessibleDesk.m_lowLevel = false;

    MSWindowsDesks desks(false, false, &screensaver, m_events.get(), new NoopJob());
    desks.m_threadID = GetCurrentThreadId();
    desks.setShape(0, 0, 1920, 1080, 960, 540, false);

    desks.checkDesk();
    QVERIFY(!desks.m_isOnComputer);

    MSWindowsDesks::Desk *realDesk = desks.m_activeDesk;
    if (!desks.isDeskAccessible(realDesk) || realDesk->m_window == nullptr) {
      return;
    }

    // Leave secondary so the blank hider window becomes visible.
    desks.leave(desks.m_keyLayout);
    QVERIFY(!desks.m_isOnComputer);

    // Simulate switching away to the inaccessible UAC desktop while off computer.
    desks.m_activeDesk = &inaccessibleDesk;
    desks.m_activeDeskName = L"";

    // Returning to the accessible desktop while still logically left keeps m_isOnComputer == false.
    desks.checkDesk();
    QVERIFY(!desks.m_isOnComputer);
    QCOMPARE(desks.m_activeDesk, realDesk);

    // Simulate switching away again, then entering secondary while on the inaccessible desktop.
    desks.m_activeDesk = &inaccessibleDesk;
    desks.m_activeDeskName = L"";
    desks.enter();
    QVERIFY(desks.m_isOnComputer);

    // Returning to Default desktop must execute deskEnter on realDesk and keep m_isOnComputer == true.
    desks.checkDesk();
    QVERIFY(desks.m_isOnComputer);
    QCOMPARE(desks.m_activeDesk, realDesk);
    QVERIFY(!IsWindowVisible(realDesk->m_window));
  }

  void defaultShapeYIsZero()
  {
    DummyScreenSaver screensaver;
    MSWindowsDesks desks(false, false, &screensaver, m_events.get(), new NoopJob());
    QCOMPARE(desks.m_y, 0);
  }

  void normalizeVirtualDesktopPoint_handlesNegativeOriginMonitors()
  {
    // Two 1920x1080 monitors side-by-side with the left monitor at x=-1920..0
    const auto topLeft = MSWindowsDesks::normalizeVirtualDesktopPoint(-1920, 0, -1920, 0, 3840, 1080);
    QCOMPARE(topLeft.first, 0u);
    QCOMPARE(topLeft.second, 0u);

    const auto bottomRight = MSWindowsDesks::normalizeVirtualDesktopPoint(1919, 1079, -1920, 0, 3840, 1080);
    QCOMPARE(bottomRight.first, 65535u);
    QCOMPARE(bottomRight.second, 65535u);

    const auto midLeft = MSWindowsDesks::normalizeVirtualDesktopPoint(-960, 540, -1920, 0, 3840, 1080);
    QCOMPARE(midLeft.first, 16388u);
    QCOMPARE(midLeft.second, 32798u);
    QVERIFY(midLeft.first <= 65535u);
    QVERIFY(midLeft.second <= 65535u);

    const auto clamped = MSWindowsDesks::normalizeVirtualDesktopPoint(-2500, 2000, -1920, 0, 3840, 1080);
    QCOMPARE(clamped.first, 0u);
    QCOMPARE(clamped.second, 65535u);
  }

private:
  Arch m_arch;
  Log m_log;
  std::unique_ptr<EventQueue> m_events;
  std::unique_ptr<AppUtilWindows> m_appUtil;
};

QTEST_MAIN(MSWindowsDesksTests)
#include "MSWindowsDesksTests.moc"
