/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ServerProxyTests.h"

#include "base/Event.h"
#include "base/IEventQueue.h"
#include "client/Client.h"
#include "client/ServerProxy.h"
#include "base/ILogOutputter.h"
#include "deskflow/AppUtil.h"
#include "deskflow/Computer.h"
#include "deskflow/IPlatformComputer.h"
#include "deskflow/ProtocolTypes.h"
#include "io/IStream.h"
#include "net/ISocketFactory.h"
#include "net/NetworkAddress.h"

#include <QTest>
#include <vector>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

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

class FakeStream : public deskflow::IStream
{
public:
  void push(const std::string &bytes)
  {
    m_chunks.push_back(bytes);
  }

  void close() override
  {
    m_chunks.clear();
    m_inputShutdown = true;
  }

  uint32_t read(void *buffer, uint32_t size) override
  {
    if (m_inputShutdown || m_chunks.empty() || size == 0) {
      return 0;
    }

    auto &front = m_chunks.front();
    const size_t bytesToRead = std::min(static_cast<size_t>(size), front.size());
    if (buffer != nullptr) {
      std::memcpy(buffer, front.data(), bytesToRead);
    }

    front.erase(0, bytesToRead);
    if (front.empty()) {
      m_chunks.pop_front();
    }
    return static_cast<uint32_t>(bytesToRead);
  }

  void write(const void *buffer, uint32_t size) override
  {
    if (buffer != nullptr && size > 0) {
      m_written.append(static_cast<const char *>(buffer), size);
    }
  }

  std::string takeWritten()
  {
    auto out = std::move(m_written);
    m_written.clear();
    return out;
  }

  void flush() override
  {
  }

  void shutdownInput() override
  {
    close();
  }

  void shutdownOutput() override
  {
  }

  void *getEventTarget() const override
  {
    return const_cast<FakeStream *>(this);
  }

  bool isReady() const override
  {
    return !m_inputShutdown && !m_chunks.empty();
  }

  uint32_t getSize() const override
  {
    size_t total = 0;
    for (const auto &chunk : m_chunks) {
      total += chunk.size();
    }
    return static_cast<uint32_t>(std::min<size_t>(total, UINT32_MAX));
  }

private:
  std::deque<std::string> m_chunks;
  std::string m_written;
  bool m_inputShutdown = false;
};

class RecordingEventQueue : public IEventQueue
{
public:
  ~RecordingEventQueue() override
  {
    for (const auto &event : m_addedEvents) {
      Event::deleteData(event);
    }
  }

  int loop() override
  {
    return 0;
  }

  void adoptBuffer(IEventQueueBuffer *) override
  {
  }

  bool getEvent(Event &, double = -1.0) override
  {
    return false;
  }

  bool dispatchEvent(const Event &event) override
  {
    const auto handler = m_handlers.find(HandlerKey{event.getType(), event.getTarget()});
    if (handler == m_handlers.end()) {
      return false;
    }

    const auto callback = handler->second;
    callback(event);
    return true;
  }

  void addEvent(Event &&event) override
  {
    m_addedEvents.emplace_back(std::move(event));
  }

  EventQueueTimer *newTimer(double, void *) override
  {
    return timer();
  }

  EventQueueTimer *newOneShotTimer(double, void *) override
  {
    return timer();
  }

  void deleteTimer(EventQueueTimer *) override
  {
  }

  void addHandler(EventTypes type, void *target, const EventHandler &handler) override
  {
    m_handlers[HandlerKey{type, target}] = handler;
  }

  void removeHandler(EventTypes type, void *target) override
  {
    m_handlers.erase(HandlerKey{type, target});
  }

  void removeHandlers(void *target) override
  {
    for (auto it = m_handlers.begin(); it != m_handlers.end();) {
      if (it->first.target == target) {
        it = m_handlers.erase(it);
      } else {
        ++it;
      }
    }
  }

  void waitForReady() const override
  {
  }

  void *getSystemTarget() override
  {
    return this;
  }

  EventQueueTimer *timer()
  {
    return reinterpret_cast<EventQueueTimer *>(&m_timerStorage);
  }

  const std::vector<Event> &addedEvents() const
  {
    return m_addedEvents;
  }

private:
  struct HandlerKey
  {
    EventTypes type;
    void *target;

    bool operator<(const HandlerKey &other) const
    {
      if (type != other.type) {
        return static_cast<uint32_t>(type) < static_cast<uint32_t>(other.type);
      }
      return std::less<void *>{}(target, other.target);
    }
  };

  int m_timerStorage = 0;
  std::map<HandlerKey, EventHandler> m_handlers;
  std::vector<Event> m_addedEvents;
};

class TestServerProxy : public ServerProxy
{
public:
  using ServerProxy::ServerProxy;

  bool parseHandshakeMessageReturnsDisconnect(const uint8_t *code)
  {
    return parseHandshakeMessage(code) == ConnectionResult::Disconnect;
  }

  bool invokeHandleMessageIsOkay(const uint8_t *code)
  {
    return handleMessage(code) == ConnectionResult::Okay;
  }
};

class NoopSocketFactory final : public ISocketFactory
{
public:
  IDataSocket *create(IArchNetwork::AddressFamily, SecurityLevel) const override
  {
    return nullptr;
  }
  IListenSocket *createListen(IArchNetwork::AddressFamily, SecurityLevel) const override
  {
    return nullptr;
  }
};

class StubPlatformComputer final : public IPlatformComputer
{
public:
  explicit StubPlatformComputer(const IEventQueue *events) : IPlatformComputer(events)
  {
  }

  void *getEventTarget() const override
  {
    return const_cast<StubPlatformComputer *>(this);
  }
  bool getClipboard(ClipboardID, IClipboard *) const override
  {
    return false;
  }
  void getShape(int32_t &x, int32_t &y, int32_t &width, int32_t &height) const override
  {
    x = 0;
    y = 0;
    width = 1920;
    height = 1080;
  }
  void getCursorPos(int32_t &x, int32_t &y) const override
  {
    x = 0;
    y = 0;
  }
  void reconfigure(uint32_t) override
  {
  }
  uint32_t activeSides() override
  {
    return 0;
  }
  void warpCursor(int32_t, int32_t) override
  {
  }
  uint32_t registerHotKey(KeyID, KeyModifierMask) override
  {
    return 0;
  }
  void unregisterHotKey(uint32_t) override
  {
  }
  void fakeInputBegin() override
  {
  }
  void fakeInputEnd() override
  {
  }
  int32_t getJumpZoneSize() const override
  {
    return 1;
  }
  bool isAnyMouseButtonDown(uint32_t &) const override
  {
    return false;
  }
  void getCursorCenter(int32_t &x, int32_t &y) const override
  {
    x = 960;
    y = 540;
  }
  void fakeMouseButton(ButtonID, bool) override
  {
  }
  void fakeMouseMove(int32_t, int32_t) override
  {
  }
  void fakeMouseRelativeMove(int32_t, int32_t) const override
  {
  }
  void fakeMouseWheel(ScrollDelta) const override
  {
  }
  void updateKeyMap() override
  {
  }
  void updateKeyState() override
  {
  }
  void setHalfDuplexMask(KeyModifierMask) override
  {
  }
  void fakeKeyDown(KeyID, KeyModifierMask, KeyButton, const std::string &) override
  {
  }
  bool fakeKeyRepeat(KeyID, KeyModifierMask, int32_t, KeyButton, const std::string &) override
  {
    return false;
  }
  bool fakeKeyUp(KeyButton) override
  {
    return false;
  }
  void fakeAllKeysUp() override
  {
  }
  bool fakeCtrlAltDel() override
  {
    return false;
  }
  bool fakeMediaKey(KeyID) override
  {
    return false;
  }
  bool isKeyDown(KeyButton) const override
  {
    return false;
  }
  KeyModifierMask getActiveModifiers() const override
  {
    return 0;
  }
  KeyModifierMask pollActiveModifiers() const override
  {
    return 0;
  }
  int32_t pollActiveGroup() const override
  {
    return 0;
  }
  void pollPressedKeys(KeyButtonSet &) const override
  {
  }
  void enable() override
  {
  }
  void disable() override
  {
  }
  void enter() override
  {
  }
  bool canLeave() override
  {
    return true;
  }
  void leave() override
  {
  }
  bool setClipboard(ClipboardID, const IClipboard *) override
  {
    return false;
  }
  void checkClipboards() override
  {
  }
  void openScreensaver(bool) override
  {
  }
  void closeScreensaver() override
  {
  }
  void screensaver(bool) override
  {
  }
  void resetOptions() override
  {
  }
  void setOptions(const OptionsList &) override
  {
  }
  void setSequenceNumber(uint32_t) override
  {
  }
  std::string getSecureInputApp() const override
  {
    return {};
  }
  bool isPrimary() const override
  {
    return false;
  }
  void handleSystemEvent(const Event &) override
  {
  }
};

Client *undereferenceableClient()
{
  // These paths must queue cleanup without calling through to Client.
  return reinterpret_cast<Client *>(0x1);
}

TestAppUtil &testAppUtil()
{
  static TestAppUtil util;
  return util;
}

const Client::DisconnectRequest *disconnectRequest(const RecordingEventQueue &events)
{
  if (events.addedEvents().size() != 1) {
    return nullptr;
  }
  return static_cast<const Client::DisconnectRequest *>(events.addedEvents().front().getDataObject());
}

} // namespace

void ServerProxyTests::initTestCase()
{
  (void)testAppUtil();
  m_log.setFilter(LogLevel::Level::Debug);
}

void ServerProxyTests::handleKeepAliveAlarm_timeout_queuesDisconnectRequest()
{
  RecordingEventQueue events;
  FakeStream stream;
  ServerProxy proxy(undereferenceableClient(), &stream, &events);

  QVERIFY(events.dispatchEvent(Event(EventTypes::Timer, events.timer())));

  QCOMPARE(events.addedEvents().size(), static_cast<size_t>(1));
  const auto &event = events.addedEvents().front();
  QVERIFY(event.getType() == EventTypes::ClientDisconnectRequested);
  QCOMPARE(event.getTarget(), stream.getEventTarget());

  const auto *request = disconnectRequest(events);
  QVERIFY(request != nullptr);
  QVERIFY(request->kind() == Client::DisconnectRequest::Kind::Disconnect);
  QCOMPARE(QString::fromUtf8(request->message()), QStringLiteral("server is not responding"));
}

void ServerProxyTests::handleData_incompleteMessage_queuesDisconnectRequest()
{
  RecordingEventQueue events;
  FakeStream stream;
  stream.push("DF");
  ServerProxy proxy(undereferenceableClient(), &stream, &events);

  QVERIFY(events.dispatchEvent(Event(EventTypes::StreamInputReady, stream.getEventTarget())));

  QCOMPARE(events.addedEvents().size(), static_cast<size_t>(1));
  const auto &event = events.addedEvents().front();
  QVERIFY(event.getType() == EventTypes::ClientDisconnectRequested);
  QCOMPARE(event.getTarget(), stream.getEventTarget());

  const auto *request = disconnectRequest(events);
  QVERIFY(request != nullptr);
  QVERIFY(request->kind() == Client::DisconnectRequest::Kind::Disconnect);
  QCOMPARE(QString::fromUtf8(request->message()), QStringLiteral("incomplete message from server"));
}

void ServerProxyTests::parseHandshakeMessage_protocolError_queuesRefusalRequest()
{
  RecordingEventQueue events;
  FakeStream stream;
  TestServerProxy proxy(undereferenceableClient(), &stream, &events);

  QVERIFY(proxy.parseHandshakeMessageReturnsDisconnect(reinterpret_cast<const uint8_t *>(kMsgEBad)));
  QCOMPARE(events.addedEvents().size(), static_cast<size_t>(1));
  const auto *request = disconnectRequest(events);
  QVERIFY(request != nullptr);
  QVERIFY(request->kind() == Client::DisconnectRequest::Kind::Refuse);
  QVERIFY(request->refusalReason() == deskflow::core::ConnectionRefusal::ProtocolError);
  QCOMPARE(QString::fromUtf8(request->message()), QStringLiteral("server reported a protocol error"));
}

void ServerProxyTests::handleMessage_doesNotSendNoopPerMouseMove()
{
  RecordingEventQueue events;
  FakeStream stream;
  auto *computer = new deskflow::Computer(new StubPlatformComputer(&events), &events);
  Client client(&events, "client", NetworkAddress("127.0.0.1", 24800), new NoopSocketFactory(), computer);
  TestServerProxy proxy(&client, &stream, &events);
  stream.takeWritten();

  // kMsgDMouseMove payload: (10, 20) in big-endian int16_t
  stream.push(std::string("\x00\x0a\x00\x14", 4));
  QVERIFY(proxy.invokeHandleMessageIsOkay(reinterpret_cast<const uint8_t *>(kMsgDMouseMove)));

  const std::string written = stream.takeWritten();
  QCOMPARE(written.find(std::string(kMsgCNoop, 4)), std::string::npos);
}

namespace {

class RecordingLogOutputter final : public ILogOutputter
{
public:
  void open(const QString &) override {}
  void close() override {}
  bool write(LogLevel::Level, const QString &message) override
  {
    m_messages.push_back(message.toStdString());
    return false;
  }

  std::vector<std::string> m_messages;
};

} // namespace

void ServerProxyTests::sendDragInfo_writesDdrgFrameAndRedactsSensitiveLogs()
{
  RecordingEventQueue events;
  FakeStream stream;
  auto *computer = new deskflow::Computer(new StubPlatformComputer(&events), &events);
  Client client(&events, "client", NetworkAddress("127.0.0.1", 24800), new NoopSocketFactory(), computer);
  TestServerProxy proxy(&client, &stream, &events);
  stream.takeWritten();

  auto *logRecorder = new RecordingLogOutputter();
  const auto prevFilter = CLOG->getFilter();
  CLOG->insert(logRecorder, true);
  CLOG->setFilter(LogLevel::Level::Info);

  const std::string secretToken = "superSecretAuthToken1234";
  const std::string payload = "DESKSEND_SEND_DROP:100,200:" + secretToken + ":192.168.1.50";
  QVERIFY(!client.sendDragInfo(1, "unconnected"));
  proxy.sendDragInfo(3, payload);

  const std::string written = stream.takeWritten();
  const uint32_t expectedLen = static_cast<uint32_t>(payload.size());
  std::string expectedFrame(kMsgDDragInfo, 4);
  expectedFrame.push_back(0x00);
  expectedFrame.push_back(0x03);
  expectedFrame.push_back(static_cast<char>((expectedLen >> 24) & 0xFF));
  expectedFrame.push_back(static_cast<char>((expectedLen >> 16) & 0xFF));
  expectedFrame.push_back(static_cast<char>((expectedLen >> 8) & 0xFF));
  expectedFrame.push_back(static_cast<char>(expectedLen & 0xFF));
  expectedFrame.append(payload);
  QCOMPARE(written, expectedFrame);

  // Also check receive-side redaction via kMsgDDragInfo
  const std::string recvPayload = "DESKSEND_SEND_CANCEL:" + secretToken;
  const uint32_t recvLen = static_cast<uint32_t>(recvPayload.size());
  std::string wireRecvPayload;
  wireRecvPayload.push_back(0x00);
  wireRecvPayload.push_back(0x01);
  wireRecvPayload.push_back(static_cast<char>((recvLen >> 24) & 0xFF));
  wireRecvPayload.push_back(static_cast<char>((recvLen >> 16) & 0xFF));
  wireRecvPayload.push_back(static_cast<char>((recvLen >> 8) & 0xFF));
  wireRecvPayload.push_back(static_cast<char>(recvLen & 0xFF));
  wireRecvPayload.append(recvPayload);
  stream.push(wireRecvPayload);
  QVERIFY(proxy.invokeHandleMessageIsOkay(reinterpret_cast<const uint8_t *>(kMsgDDragInfo)));

  CLOG->remove(logRecorder);
  CLOG->setFilter(prevFilter);
  bool sawRedacted = false;
  for (const auto &line : logRecorder->m_messages) {
    QCOMPARE(line.find(secretToken), std::string::npos);
    if (line.find("<redacted>") != std::string::npos) {
      sawRedacted = true;
    }
  }
  delete logRecorder;
  QVERIFY(sawRedacted);
}

QTEST_MAIN(ServerProxyTests)
