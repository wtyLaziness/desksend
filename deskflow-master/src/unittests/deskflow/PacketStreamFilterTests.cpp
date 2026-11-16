/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "PacketStreamFilterTests.h"

#include "MockEventQueue.h"
#include "deskflow/PacketStreamFilter.h"
#include "io/IStream.h"

#include <QByteArray>
#include <QTest>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>

namespace {

class RecordingStream final : public deskflow::IStream
{
public:
  void write(const void *buffer, uint32_t n) override
  {
    ++m_writeCalls;
    QByteArray chunk(static_cast<const char *>(buffer), static_cast<qsizetype>(n));
    m_chunks.push_back(chunk);
    m_incoming.push_back(std::string(chunk.constData(), static_cast<size_t>(chunk.size())));
  }

  uint32_t read(void *buffer, uint32_t n) override
  {
    if (m_incoming.empty() || n == 0) {
      return 0;
    }
    auto &front = m_incoming.front();
    const size_t take = std::min<size_t>(n, front.size());
    if (buffer != nullptr) {
      std::memcpy(buffer, front.data(), take);
    }
    front.erase(0, take);
    if (front.empty()) {
      m_incoming.pop_front();
    }
    return static_cast<uint32_t>(take);
  }

  void close() override
  {
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
    return const_cast<RecordingStream *>(this);
  }
  bool isReady() const override
  {
    return !m_incoming.empty();
  }
  uint32_t getSize() const override
  {
    return 0;
  }

  int writeCalls() const
  {
    return m_writeCalls;
  }
  const std::vector<QByteArray> &chunks() const
  {
    return m_chunks;
  }
  void clear()
  {
    m_writeCalls = 0;
    m_chunks.clear();
    m_incoming.clear();
  }

private:
  int m_writeCalls = 0;
  std::vector<QByteArray> m_chunks;
  std::deque<std::string> m_incoming;
};

} // namespace

void PacketStreamFilterTests::initTestCase()
{
  m_log.setFilter(LogLevel::Level::Debug);
}

void PacketStreamFilterTests::packetStreamFilterWriteWritesSingleFrame()
{
  MockEventQueue events;
  RecordingStream stream;
  PacketStreamFilter filter(&events, &stream, false);

  filter.write("abcd", 4);
  QCOMPARE(stream.writeCalls(), 1);
  QCOMPARE(stream.chunks().size(), static_cast<size_t>(1));
  QCOMPARE(stream.chunks().front(), QByteArray("\x00\x00\x00\x04" "abcd", 8));

  // Verify large payload > 4096 stack buffer also emits as a single underlying write
  stream.clear();
  const std::string largePayload(5000, 'x');
  filter.write(largePayload.data(), static_cast<uint32_t>(largePayload.size()));
  QCOMPARE(stream.writeCalls(), 1);
  QCOMPARE(stream.chunks().front().size(), 5004);
  QCOMPARE(stream.chunks().front().left(4), QByteArray("\x00\x00\x13\x88", 4));
  QCOMPARE(stream.chunks().front().mid(4), QByteArray(largePayload.data(), 5000));
}

QTEST_MAIN(PacketStreamFilterTests)
