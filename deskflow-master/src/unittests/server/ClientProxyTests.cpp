/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ClientProxyTests.h"

#include "../deskflow/MockEventQueue.h"
#include "deskflow/AppUtil.h"
#include "io/IStream.h"
#include "server/ClientProxy1_0.h"
#include "server/ClientProxy1_1.h"
#include "server/ClientProxy1_6.h"
#include "server/ClientProxy1_7.h"
#include "server/ClientProxy1_8.h"
#include "deskflow/ProtocolTypes.h"
#include "io/IStream.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <QByteArray>
#include <QTest>

namespace {

class TestAppUtil : public AppUtil
{
public:
  int run() override
  {
    return 0;
  }

  std::vector<std::string> getKeyboardLayoutList() override
  {
    return {"en"};
  }

  std::string getCurrentLanguageCode() override
  {
    return "en";
  }
};

class CapturingStream : public deskflow::IStream
{
public:
  QByteArray take()
  {
    auto bytes = m_buffer;
    m_buffer.clear();
    return bytes;
  }

  void write(const void *buffer, uint32_t n) override
  {
    m_buffer.append(static_cast<const char *>(buffer), n);
  }

  void close() override
  {
  }

  uint32_t read(void *, uint32_t) override
  {
    return 0;
  }

  void flush() override
  {
  }

  void shutdownInput() override
  {
  }

  void shutdownOutput() override
  {
  }

  void *getEventTarget() const override
  {
    return const_cast<CapturingStream *>(this);
  }

  bool isReady() const override
  {
    return false;
  }

  uint32_t getSize() const override
  {
    return 0;
  }

private:
  QByteArray m_buffer;
};

std::unique_ptr<ClientProxy> makeProxy(int minor, deskflow::IStream *stream, IEventQueue *events)
{
  // the 1.4 and later constructors assert the server pointer is non-null but only store it
  auto *server = reinterpret_cast<Server *>(0x1);

  std::unique_ptr<ClientProxy> proxy;
  switch (minor) {
  case 0:
    proxy = std::make_unique<ClientProxy1_0>("client", stream, events);
    break;
  case 1:
    proxy = std::make_unique<ClientProxy1_1>("client", stream, events);
    break;
  case 6:
    proxy = std::make_unique<ClientProxy1_6>("client", stream, server, events);
    break;
  case 7:
    proxy = std::make_unique<ClientProxy1_7>("client", stream, server, events);
    break;
  case 8:
    proxy = std::make_unique<ClientProxy1_8>("client", stream, server, events);
    break;
  default:
    break;
  }
  return proxy;
}

struct ProxyUnderTest
{
  MockEventQueue events;
  CapturingStream *stream = new CapturingStream;
  std::unique_ptr<ClientProxy> proxy;

  explicit ProxyUnderTest(int minor) : proxy(makeProxy(minor, stream, &events))
  {
    // drop the query-info and layout-sync messages the constructors send
    stream->take();
  }
};

const KeyID kKey = 0x61;
const KeyModifierMask kMask = 0;
const KeyButton kButton = 0x1e;
const int32_t kCount = 3;
const std::string kLang = "en";

} // namespace

void ClientProxyTests::initTestCase()
{
  // the 1.8 constructor reads the keyboard layouts through AppUtil::instance()
  static TestAppUtil appUtil;
}

// These formats are frozen because shipped third-party clients parse them byte
// for byte: Synergy 1.4 through 1.14.1 negotiate 1.4 through 1.7, Barrier and
// Input Leap negotiate 1.6, and Synergy 1.14.2 onwards and Deskflow negotiate 1.8.
void ClientProxyTests::keyDown_data()
{
  QTest::addColumn<int>("minor");
  QTest::addColumn<QByteArray>("expected");

  QTest::newRow("1.0") << 0 << "DKDN" + QByteArray::fromHex("0061 0000");
  QTest::newRow("1.1") << 1 << "DKDN" + QByteArray::fromHex("0061 0000 001e");
  QTest::newRow("1.6") << 6 << "DKDN" + QByteArray::fromHex("0061 0000 001e");
  QTest::newRow("1.7") << 7 << "DKDN" + QByteArray::fromHex("0061 0000 001e");
  QTest::newRow("1.8") << 8 << "DKDL" + QByteArray::fromHex("0061 0000 001e 00000002") + "en";
}

void ClientProxyTests::keyDown()
{
  QFETCH(int, minor);
  QFETCH(QByteArray, expected);

  ProxyUnderTest test(minor);
  test.proxy->keyDown(kKey, kMask, kButton, kLang);
  QCOMPARE(test.stream->take(), expected);
}

void ClientProxyTests::keyRepeat_data()
{
  QTest::addColumn<int>("minor");
  QTest::addColumn<QByteArray>("expected");

  QTest::newRow("1.0") << 0 << "DKRP" + QByteArray::fromHex("0061 0000 0003");
  QTest::newRow("1.1") << 1 << "DKRP" + QByteArray::fromHex("0061 0000 0003 001e");
  QTest::newRow("1.6") << 6 << "DKRP" + QByteArray::fromHex("0061 0000 0003 001e");
  QTest::newRow("1.7") << 7 << "DKRP" + QByteArray::fromHex("0061 0000 0003 001e");
  QTest::newRow("1.8") << 8 << "DKRP" + QByteArray::fromHex("0061 0000 0003 001e 00000002") + "en";
}

void ClientProxyTests::keyRepeat()
{
  QFETCH(int, minor);
  QFETCH(QByteArray, expected);

  ProxyUnderTest test(minor);
  test.proxy->keyRepeat(kKey, kMask, kCount, kButton, kLang);
  QCOMPARE(test.stream->take(), expected);
}

void ClientProxyTests::keyUp_data()
{
  QTest::addColumn<int>("minor");
  QTest::addColumn<QByteArray>("expected");

  QTest::newRow("1.0") << 0 << "DKUP" + QByteArray::fromHex("0061 0000");
  QTest::newRow("1.1") << 1 << "DKUP" + QByteArray::fromHex("0061 0000 001e");
  QTest::newRow("1.6") << 6 << "DKUP" + QByteArray::fromHex("0061 0000 001e");
  QTest::newRow("1.7") << 7 << "DKUP" + QByteArray::fromHex("0061 0000 001e");
  QTest::newRow("1.8") << 8 << "DKUP" + QByteArray::fromHex("0061 0000 001e");
}

void ClientProxyTests::keyUp()
{
  QFETCH(int, minor);
  QFETCH(QByteArray, expected);

  ProxyUnderTest test(minor);
  test.proxy->keyUp(kKey, kMask, kButton);
  QCOMPARE(test.stream->take(), expected);
}

namespace {

class CountingEventQueue final : public MockEventQueue
{
public:
  EventQueueTimer *newTimer(double, void *) override
  {
    ++m_newTimerCalls;
    auto *timer = reinterpret_cast<EventQueueTimer *>(static_cast<uintptr_t>(++m_nextTimerId));
    m_activeTimers.insert(timer);
    m_lastPeriodicTimer = timer;
    return timer;
  }

  EventQueueTimer *newOneShotTimer(double, void *) override
  {
    ++m_newOneShotTimerCalls;
    auto *timer = reinterpret_cast<EventQueueTimer *>(static_cast<uintptr_t>(++m_nextTimerId));
    m_activeTimers.insert(timer);
    return timer;
  }

  void deleteTimer(EventQueueTimer *timer) override
  {
    ++m_deleteTimerCalls;
    m_activeTimers.erase(timer);
    if (m_lastPeriodicTimer == timer) {
      m_lastPeriodicTimer = nullptr;
    }
  }

  void addHandler(EventTypes type, void *target, const EventHandler &handler) override
  {
    if (type == EventTypes::Timer && target != nullptr) {
      m_timerHandlers[target] = handler;
    }
  }

  void removeHandler(EventTypes type, void *target) override
  {
    if (type == EventTypes::Timer) {
      m_timerHandlers.erase(target);
    }
  }

  void addEvent(Event &&event) override
  {
    if (event.getType() == EventTypes::ClientProxyDisconnected) {
      ++m_disconnectEvents;
    }
    Event::deleteData(event);
  }

  bool fireTimer(void *target)
  {
    if (target == nullptr) {
      return false;
    }
    auto it = m_timerHandlers.find(target);
    if (it == m_timerHandlers.end()) {
      return false;
    }
    const auto cb = it->second;
    cb(Event(EventTypes::Timer, target));
    return true;
  }

  int newTimerCalls() const
  {
    return m_newTimerCalls;
  }
  int newOneShotTimerCalls() const
  {
    return m_newOneShotTimerCalls;
  }
  int deleteTimerCalls() const
  {
    return m_deleteTimerCalls;
  }
  size_t activeTimerCount() const
  {
    return m_activeTimers.size();
  }
  bool isTimerActive(EventQueueTimer *timer) const
  {
    return timer != nullptr && m_activeTimers.find(timer) != m_activeTimers.end();
  }
  EventQueueTimer *lastPeriodicTimer() const
  {
    return m_lastPeriodicTimer;
  }
  int disconnectEvents() const
  {
    return m_disconnectEvents;
  }

private:
  uintptr_t m_nextTimerId = 0x1000;
  int m_newTimerCalls = 0;
  int m_newOneShotTimerCalls = 0;
  int m_deleteTimerCalls = 0;
  int m_disconnectEvents = 0;
  EventQueueTimer *m_lastPeriodicTimer = nullptr;
  std::set<EventQueueTimer *> m_activeTimers;
  std::map<void *, EventHandler> m_timerHandlers;
};

class TestClientProxy1_6 final : public ClientProxy1_6
{
public:
  using ClientProxy1_6::ClientProxy1_6;
  using ClientProxy1_6::addHeartbeatTimer;
  using ClientProxy1_6::removeHeartbeatTimer;
  using ClientProxy1_6::resetHeartbeatTimer;
  using ClientProxy1_6::setHeartbeatRate;
};

} // namespace

void ClientProxyTests::resetHeartbeatTimer_reusesTimersWithoutChurn()
{
  CountingEventQueue events;
  auto *stream = new CapturingStream;
  auto *server = reinterpret_cast<Server *>(0x1);
  TestClientProxy1_6 proxy("client", stream, server, &events);
  proxy.setHeartbeatRate(1.0, 5.0);
  proxy.addHeartbeatTimer();

  const int initialNewCalls = events.newOneShotTimerCalls();
  QCOMPARE(events.deleteTimerCalls(), 0);
  QCOMPARE(events.activeTimerCount(), static_cast<size_t>(2));

  for (int i = 0; i < 50; ++i) {
    proxy.resetHeartbeatTimer();
  }

  QCOMPARE(events.deleteTimerCalls(), 0);
  QCOMPARE(events.newOneShotTimerCalls(), initialNewCalls);
  QCOMPARE(events.activeTimerCount(), static_cast<size_t>(2));

  // Firing the timer right after activity must re-arm for remaining alarm without disconnecting
  QVERIFY(events.fireTimer(&proxy));
  QCOMPARE(events.disconnectEvents(), 0);
  QCOMPARE(events.activeTimerCount(), static_cast<size_t>(2));

  // Firing again with no intervening activity must mark the client flatlined
  QVERIFY(events.fireTimer(&proxy));
  QCOMPARE(events.disconnectEvents(), 1);
}

void ClientProxyTests::addHeartbeatTimer_preservesKeepAliveTimerAndSendsCalv()
{
  CountingEventQueue events;
  auto *stream = new CapturingStream;
  auto *server = reinterpret_cast<Server *>(0x1);
  TestClientProxy1_6 proxy("client", stream, server, &events);
  stream->take();

  proxy.setHeartbeatRate(3.0, 9.0);
  proxy.addHeartbeatTimer();

  EventQueueTimer *keepAliveTimer = events.lastPeriodicTimer();
  QCOMPARE(events.newTimerCalls(), 1);
  QCOMPARE(events.newOneShotTimerCalls(), 1);
  QCOMPARE(events.deleteTimerCalls(), 0);
  QCOMPARE(events.activeTimerCount(), static_cast<size_t>(2));
  QVERIFY(events.isTimerActive(keepAliveTimer));

  QVERIFY(events.fireTimer(keepAliveTimer));
  QCOMPARE(stream->take(), QByteArray(kMsgCKeepAlive, 4));

  // Re-arming the alarm timer via resetHeartbeatTimer() + handleHeartbeatTimer()
  // must preserve the ClientProxy1_3 periodic keepalive timer.
  proxy.resetHeartbeatTimer();
  QVERIFY(events.fireTimer(&proxy));
  QCOMPARE(events.disconnectEvents(), 0);
  QVERIFY(events.isTimerActive(keepAliveTimer));
  QCOMPARE(events.activeTimerCount(), static_cast<size_t>(2));

  QVERIFY(events.fireTimer(keepAliveTimer));
  QCOMPARE(stream->take(), QByteArray(kMsgCKeepAlive, 4));

  proxy.removeHeartbeatTimer();
  QCOMPARE(events.activeTimerCount(), static_cast<size_t>(0));
  QVERIFY(!events.isTimerActive(keepAliveTimer));
}

namespace {

class ReadableCaptureStream final : public deskflow::IStream
{
public:
  void push(const std::string &bytes)
  {
    m_in.append(bytes);
  }

  void close() override {}

  uint32_t read(void *buffer, uint32_t n) override
  {
    const uint32_t avail = static_cast<uint32_t>(m_in.size() - m_pos);
    const uint32_t count = std::min(n, avail);
    if (count > 0) {
      if (buffer != nullptr) {
        memcpy(buffer, m_in.data() + m_pos, count);
      }
      m_pos += count;
    }
    return count;
  }

  void write(const void *buffer, uint32_t n) override
  {
    if (buffer != nullptr && n > 0) {
      m_out.append(static_cast<const char *>(buffer), n);
    }
  }

  void flush() override {}
  void shutdownInput() override {}
  void shutdownOutput() override {}
  void *getEventTarget() const override
  {
    return const_cast<ReadableCaptureStream *>(this);
  }
  bool isReady() const override
  {
    return m_pos < m_in.size();
  }
  uint32_t getSize() const override
  {
    return static_cast<uint32_t>(m_in.size() - m_pos);
  }

private:
  std::string m_in;
  size_t m_pos = 0;
  std::string m_out;
};

class TestDragClientProxy1_5 final : public ClientProxy1_5
{
public:
  using ClientProxy1_5::ClientProxy1_5;
  using ClientProxy1_5::parseMessage;

  std::vector<std::pair<uint16_t, std::string>> receivedDragInfos;

protected:
  void handleDragInfoMessage(uint16_t fileCount, const std::string &dragInfo) override
  {
    receivedDragInfos.emplace_back(fileCount, dragInfo);
  }
};

} // namespace

void ClientProxyTests::dragInfoReceived_consumesStreamBeforeNextKeepAlive()
{
  MockEventQueue events;
  auto *stream = new ReadableCaptureStream;
  auto *server = reinterpret_cast<Server *>(0x1);
  TestDragClientProxy1_5 proxy("client", stream, server, &events);

  const std::string info = "DESKSEND_CLIENT_PRECAPTURE:R:1919,500";
  const uint32_t infoLen = static_cast<uint32_t>(info.size());
  std::string payload;
  payload.push_back(0x00);
  payload.push_back(0x02);
  payload.push_back(static_cast<char>((infoLen >> 24) & 0xFF));
  payload.push_back(static_cast<char>((infoLen >> 16) & 0xFF));
  payload.push_back(static_cast<char>((infoLen >> 8) & 0xFF));
  payload.push_back(static_cast<char>(infoLen & 0xFF));
  payload.append(info);
  payload.append(kMsgCKeepAlive, 4);
  stream->push(payload);

  QVERIFY(proxy.parseMessage(reinterpret_cast<const uint8_t *>(kMsgDDragInfo)));
  QCOMPARE(proxy.receivedDragInfos.size(), static_cast<size_t>(1));
  QCOMPARE(proxy.receivedDragInfos[0].first, static_cast<uint16_t>(2));
  QCOMPARE(proxy.receivedDragInfos[0].second, info);

  uint8_t nextCode[4] = {};
  QCOMPARE(stream->read(nextCode, 4), static_cast<uint32_t>(4));
  QCOMPARE(std::string(reinterpret_cast<const char *>(nextCode), 4), std::string(kMsgCKeepAlive, 4));
  QVERIFY(proxy.parseMessage(nextCode));
}

QTEST_MAIN(ClientProxyTests)
