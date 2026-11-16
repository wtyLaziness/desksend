/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2014 - 2016 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ServerTests.h"

#include "server/Server.h"
#include "base/DirectionTypes.h"
#include "deskflow/DragProtocol.h"
#include "server/Config.h"

#include <algorithm>
#include <unordered_set>

void ServerTests::SwitchToComputerInfo_alloc_computer()
{
  auto actual = new Server::SwitchToComputerInfo("test");
  QCOMPARE(actual->m_computer, "test");
  delete actual;
}

void ServerTests::KeyboardBroadcastInfo_alloc_stateAndComputers()
{
  auto info = new Server::KeyboardBroadcastInfo(Server::KeyboardBroadcastInfo::State::kOn, "test");
  QCOMPARE(info->m_state, Server::KeyboardBroadcastInfo::State::kOn);
  QCOMPARE(info->m_computers, "test");
  delete info;
}

void ServerTests::bottomEdgeSwitchWaitClearsInsideBottomZone()
{
  using enum Direction;
  // 1920x1080 screen at (0,0) with 1px jump zone
  // Bottom edge jump pixel is y = 1079 -> must NOT clear wait; y = 1078 is inside -> MUST clear wait.
  QVERIFY(!Server::shouldClearSwitchWaitForDirection(Bottom, 960, 1079, 0, 0, 1920, 1080, 1));
  QVERIFY(Server::shouldClearSwitchWaitForDirection(Bottom, 960, 1078, 0, 0, 1920, 1080, 1));

  // Verify remaining 3 edges for symmetry
  QVERIFY(!Server::shouldClearSwitchWaitForDirection(Left, 0, 540, 0, 0, 1920, 1080, 1));
  QVERIFY(Server::shouldClearSwitchWaitForDirection(Left, 1, 540, 0, 0, 1920, 1080, 1));
  QVERIFY(!Server::shouldClearSwitchWaitForDirection(Right, 1919, 540, 0, 0, 1920, 1080, 1));
  QVERIFY(Server::shouldClearSwitchWaitForDirection(Right, 1918, 540, 0, 0, 1920, 1080, 1));
  QVERIFY(!Server::shouldClearSwitchWaitForDirection(Top, 960, 0, 0, 0, 1920, 1080, 1));
  QVERIFY(Server::shouldClearSwitchWaitForDirection(Top, 960, 1, 0, 0, 1920, 1080, 1));
}

void ServerTests::computeClientDragSegments_excludesDisconnectedNeighborsAndCorners()
{
  using deskflow::DragEdgeSegment;
  using deskflow::DragEdgeSide;
  using enum Direction;

  deskflow::server::Config config(nullptr);
  QVERIFY(config.addComputer("primary"));
  QVERIFY(config.addComputer("clientA"));
  QVERIFY(config.addComputer("clientB"));
  QVERIFY(config.addComputer("clientOffline"));

  QVERIFY(config.connect("clientA", Right, 0.0f, 1.0f, "primary", 0.0f, 1.0f));
  QVERIFY(config.connect("clientA", Bottom, 0.0f, 1.0f, "clientOffline", 0.0f, 1.0f));
  QVERIFY(config.connect("clientA", Top, 0.25f, 0.75f, "clientB", 0.25f, 0.75f));

  const std::unordered_set<std::string> partialConnected = {"primary", "clientA"};
  const auto partialSegs = Server::computeDragEdgeSegments(config, "clientA", partialConnected, 1920, 1080, 16);
  QCOMPARE(partialSegs.size(), size_t{1});
  QCOMPARE(partialSegs[0], (DragEdgeSegment{DragEdgeSide::Right, 16, 1064}));

  const std::unordered_set<std::string> fullConnected = {"primary", "clientA", "clientB"};
  const auto fullSegs = Server::computeDragEdgeSegments(config, "clientA", fullConnected, 1920, 1080, 16);
  QCOMPARE(fullSegs.size(), size_t{2});
  QVERIFY(std::find(fullSegs.begin(), fullSegs.end(), DragEdgeSegment{DragEdgeSide::Right, 16, 1064}) != fullSegs.end());
  QVERIFY(std::find(fullSegs.begin(), fullSegs.end(), DragEdgeSegment{DragEdgeSide::Top, 480, 1440}) != fullSegs.end());
}

QTEST_MAIN(ServerTests)
