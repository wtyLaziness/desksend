/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2004 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "deskflow/PacketStreamFilter.h"
#include "base/IEventQueue.h"
#include "deskflow/ProtocolTypes.h"

#include <array>
#include <cstring>
#include <vector>

//
// PacketStreamFilter
//

PacketStreamFilter::PacketStreamFilter(IEventQueue *events, deskflow::IStream *stream, bool adoptStream)
    : StreamFilter(events, stream, adoptStream),
      m_events(events)
{
  // do nothing
}

PacketStreamFilter::ReadFrame::ReadFrame(deskflow::IStream *stream)
    : m_filter(dynamic_cast<PacketStreamFilter *>(stream))
{
  if (m_filter) {
    std::scoped_lock lock{m_filter->m_mutex};
    ++m_filter->m_readFrameDepth;
  }
}

PacketStreamFilter::ReadFrame::~ReadFrame()
{
  if (!m_filter)
    return;
  std::scoped_lock lock{m_filter->m_mutex};
  if (--m_filter->m_readFrameDepth == 0 && m_filter->m_size == 0) {
    m_filter->readPacketSize();
    if (m_filter->m_inputShutdown && m_filter->m_size == 0)
      m_filter->m_events->addEvent(Event(EventTypes::StreamInputShutdown, m_filter->getEventTarget()));
  }
}

void PacketStreamFilter::close()
{
  std::scoped_lock lock{m_mutex};
  m_size = 0;
  m_buffer.pop(m_buffer.getSize());
  StreamFilter::close();
}

uint32_t PacketStreamFilter::read(void *buffer, uint32_t n)
{
  if (n == 0) {
    return 0;
  }

  std::scoped_lock lock{m_mutex};

  // if not enough data yet then give up
  if (!isReadyNoLock()) {
    return 0;
  }

  // read no more than what's left in the buffered packet
  if (n > m_size) {
    n = m_size;
  }

  // read it
  if (buffer != nullptr) {
    memcpy(buffer, m_buffer.peek(n), n);
  }
  m_buffer.pop(n);
  m_size -= n;

  // get next packet's size if we've finished with this packet and
  // there's enough data to do so.
  readPacketSize();

  if (m_readFrameDepth == 0 && m_inputShutdown && m_size == 0) {
    m_events->addEvent(Event(EventTypes::StreamInputShutdown, getEventTarget()));
  }

  return n;
}

void PacketStreamFilter::write(const void *buffer, uint32_t count)
{
  const size_t total = sizeof(uint32_t) + static_cast<size_t>(count);
  auto writeHeader = [count](uint8_t *dst) {
    dst[0] = static_cast<uint8_t>((count >> 24) & 0xff);
    dst[1] = static_cast<uint8_t>((count >> 16) & 0xff);
    dst[2] = static_cast<uint8_t>((count >> 8) & 0xff);
    dst[3] = static_cast<uint8_t>(count & 0xff);
  };

  // 将 4 字节包长与负载合并为单次底层写入，避免短输入包触发双倍 TLS 记录开销。
  std::array<uint8_t, 4096> stackBuffer;
  if (total <= stackBuffer.size()) {
    writeHeader(stackBuffer.data());
    if (count > 0 && buffer != nullptr) {
      std::memcpy(stackBuffer.data() + sizeof(uint32_t), buffer, count);
    }
    getStream()->write(stackBuffer.data(), static_cast<uint32_t>(total));
    return;
  }

  std::vector<uint8_t> heapBuffer(total);
  writeHeader(heapBuffer.data());
  if (count > 0 && buffer != nullptr) {
    std::memcpy(heapBuffer.data() + sizeof(uint32_t), buffer, count);
  }
  getStream()->write(heapBuffer.data(), static_cast<uint32_t>(total));
}

void PacketStreamFilter::shutdownInput()
{
  std::scoped_lock lock{m_mutex};
  m_size = 0;
  m_buffer.pop(m_buffer.getSize());
  StreamFilter::shutdownInput();
}

bool PacketStreamFilter::isReady() const
{
  std::scoped_lock lock{m_mutex};
  return isReadyNoLock();
}

uint32_t PacketStreamFilter::getSize() const
{
  std::scoped_lock lock{m_mutex};
  return isReadyNoLock() ? m_size : 0;
}

bool PacketStreamFilter::isReadyNoLock() const
{
  return m_size != 0 && m_size <= PROTOCOL_MAX_MESSAGE_LENGTH && m_buffer.getSize() >= m_size;
}

bool PacketStreamFilter::readPacketSize()
{
  // note -- m_mutex must be locked on entry

  if (m_readFrameDepth == 0 && m_size == 0 && m_buffer.getSize() >= 4) {
    uint8_t buffer[4];
    memcpy(buffer, m_buffer.peek(sizeof(buffer)), sizeof(buffer));
    m_buffer.pop(sizeof(buffer));
    m_size =
        ((uint32_t)buffer[0] << 24) | ((uint32_t)buffer[1] << 16) | ((uint32_t)buffer[2] << 8) | (uint32_t)buffer[3];
    if (m_size > PROTOCOL_MAX_MESSAGE_LENGTH) {
      m_events->addEvent(Event(EventTypes::StreamInputFormatError, getEventTarget()));
      return false;
    }
  }
  return true;
}

bool PacketStreamFilter::readMore()
{
  // note if we have whole packet
  bool wasReady = isReadyNoLock();

  // read more data
  char buffer[4096];
  uint32_t n = getStream()->read(buffer, sizeof(buffer));
  while (n > 0) {
    m_buffer.write(buffer, n);

    // if we don't yet have the next packet size then get it, if possible.
    // Note that we can't wait for whole pending data to arrive because it may be huge in
    // case of malicious or erroneous peer.
    if (!readPacketSize()) {
      break;
    }

    n = getStream()->read(buffer, sizeof(buffer));
  }

  // note if we now have a whole packet
  bool isReady = isReadyNoLock();

  // if we weren't ready before but now we are then send a
  // input ready event apparently from the filtered stream.
  return (wasReady != isReady);
}

void PacketStreamFilter::filterEvent(const Event &event)
{
  if (event.getType() == EventTypes::StreamInputReady) {
    std::scoped_lock lock{m_mutex};
    if (!readMore()) {
      return;
    }
  } else if (event.getType() == EventTypes::StreamInputShutdown) {
    // discard this if we have buffered data
    std::scoped_lock lock{m_mutex};
    m_inputShutdown = true;
    if (m_size != 0) {
      if (m_buffer.getSize() >= m_size) {
        // we have a complete packet, so we can process it before
        // shutting down.
        return;
      }
      // we have a partial packet, but the stream has shut down.
      // we'll never get the rest of the packet, so we should
      // signal an error and then shut down.
      m_events->addEvent(Event(EventTypes::StreamInputFormatError, getEventTarget()));
      m_size = 0;
    }
  }

  // pass event
  StreamFilter::filterEvent(event);
}
