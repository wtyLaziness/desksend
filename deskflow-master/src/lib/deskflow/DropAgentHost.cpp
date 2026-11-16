/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 DeskSend Contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "deskflow/DropAgentHost.h"

#include "base/ILogOutputter.h"
#include "base/Log.h"
#include "deskflow/DropAgentIpc.h"
#include "deskflow/DropExecutor.h"

#include <QCoreApplication>
#include <QMetaObject>

#include <array>
#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace deskflow {

namespace {

class StdPipeWriter
{
public:
  StdPipeWriter()
  {
#if defined(_WIN32)
    m_stdoutHandle = GetStdHandle(STD_OUTPUT_HANDLE);
#endif
  }

  void writeEvent(const DropAgentEvent &ev)
  {
    std::string line = serializeDropAgentEvent(ev);
    line.push_back('\n');

    std::scoped_lock lock(m_mutex);
#if defined(_WIN32)
    if (m_stdoutHandle != nullptr && m_stdoutHandle != INVALID_HANDLE_VALUE) {
      DWORD totalWritten = 0;
      const DWORD len = static_cast<DWORD>(line.size());
      while (totalWritten < len) {
        DWORD written = 0;
        if (!WriteFile(m_stdoutHandle, line.data() + totalWritten, len - totalWritten, &written, nullptr) ||
            written == 0) {
          break;
        }
        totalWritten += written;
      }
    }
#else
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fflush(stdout);
#endif
  }

private:
  std::mutex m_mutex;
#if defined(_WIN32)
  HANDLE m_stdoutHandle = nullptr;
#endif
};

class PipeLogOutputter final : public ILogOutputter
{
public:
  explicit PipeLogOutputter(StdPipeWriter &writer) : m_writer(writer) {}

  void open(const QString &) override {}
  void close() override {}

  bool write(LogLevel::Level level, const QString &message) override
  {
    DropAgentEvent ev;
    ev.type = DropAgentEventType::LogMessage;
    ev.level = static_cast<int>(level);
    ev.text = message.toStdString();
    m_writer.writeEvent(ev);
    // Stop propagation to console outputter so plain text never mixes onto JSON stdout pipe.
    return true;
  }

private:
  StdPipeWriter &m_writer;
};

class PipeExecutorHost final : public IDropExecutorHost
{
public:
  explicit PipeExecutorHost(StdPipeWriter &writer) : m_writer(writer) {}

  void onPendingDragCaptured(std::vector<std::string> files, int sideIndex, int32_t x, int32_t y) override
  {
    DropAgentEvent ev;
    ev.type = DropAgentEventType::PendingDragCaptured;
    ev.files = std::move(files);
    ev.sideIndex = sideIndex;
    ev.x = x;
    ev.y = y;
    m_writer.writeEvent(ev);
  }

  void onDragCancellationCompleted(bool stillPressed) override
  {
    DropAgentEvent ev;
    ev.type = DropAgentEventType::DragCancellationCompleted;
    ev.flag = stillPressed;
    m_writer.writeEvent(ev);
  }

  void onPendingDragCleared() override
  {
    DropAgentEvent ev;
    ev.type = DropAgentEventType::PendingDragCleared;
    m_writer.writeEvent(ev);
  }

  void onActiveTransfersChanged(bool active) override
  {
    DropAgentEvent ev;
    ev.type = DropAgentEventType::ActiveTransfersChanged;
    ev.flag = active;
    m_writer.writeEvent(ev);
  }

  void onSendCompleted(const std::string &targetIp, bool success) override
  {
    DropAgentEvent ev;
    ev.type = DropAgentEventType::SendCompleted;
    ev.text = targetIp;
    ev.flag = success;
    m_writer.writeEvent(ev);
  }

  void onExecutorLog(int level, const std::string &message) override
  {
    DropAgentEvent ev;
    ev.type = DropAgentEventType::LogMessage;
    ev.level = level;
    ev.text = message;
    m_writer.writeEvent(ev);
  }

private:
  StdPipeWriter &m_writer;
};

} // namespace

int runDropAgentMain()
{
  auto *app = QCoreApplication::instance();
  if (!app) {
    return 1;
  }

  StdPipeWriter writer;
  auto *logOutputter = new PipeLogOutputter(writer);
  CLOG->insert(logOutputter);

  int rc = 0;
  {
    PipeExecutorHost host(writer);
    InProcessDropExecutor executor(true);
    executor.setHost(&host);

    std::atomic<bool> stopping{false};
    std::thread stdinReader([&]() {
#if defined(_WIN32)
      HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
#endif
      std::string buffer;
      std::array<char, 4096> chunk{};
      bool exitRequested = false;

      while (!stopping.load() && !exitRequested) {
#if defined(_WIN32)
        DWORD bytesRead = 0;
        if (hStdin == nullptr || hStdin == INVALID_HANDLE_VALUE ||
            !ReadFile(hStdin, chunk.data(), static_cast<DWORD>(chunk.size()), &bytesRead, nullptr) || bytesRead == 0) {
          break;
        }
        buffer.append(chunk.data(), bytesRead);
#else
        const size_t bytesRead = std::fread(chunk.data(), 1, chunk.size(), stdin);
        if (bytesRead == 0) {
          break;
        }
        buffer.append(chunk.data(), bytesRead);
#endif
        size_t pos = 0;
        while (true) {
          const size_t nl = buffer.find('\n', pos);
          if (nl == std::string::npos) {
            break;
          }
          std::string line = buffer.substr(pos, nl - pos);
          pos = nl + 1;
          if (!line.empty() && line.back() == '\r') {
            line.pop_back();
          }
          if (line.empty()) {
            continue;
          }
          if (auto cmd = deserializeDropAgentCommand(line); cmd.has_value()) {
            if (cmd->type == DropAgentCommandType::Shutdown) {
              exitRequested = true;
              break;
            }
            const DropAgentCommand dispatched = std::move(*cmd);
            QMetaObject::invokeMethod(
                app, [&executor, dispatched]() { executor.execute(dispatched); }, Qt::QueuedConnection
            );
          }
        }
        if (pos > 0) {
          buffer.erase(0, pos);
        }
      }

      stopping.store(true);
      QMetaObject::invokeMethod(
          app,
          [&executor, app]() {
            DropAgentCommand shutdownCmd;
            shutdownCmd.type = DropAgentCommandType::Shutdown;
            executor.execute(shutdownCmd);
            app->quit();
          },
          Qt::QueuedConnection
      );
    });

    DropAgentEvent readyEv;
    readyEv.type = DropAgentEventType::Ready;
    writer.writeEvent(readyEv);

    rc = QCoreApplication::exec();
    stopping.store(true);
#if defined(_WIN32)
    CancelSynchronousIo(static_cast<HANDLE>(stdinReader.native_handle()));
#endif
    if (stdinReader.joinable()) {
      stdinReader.join();
    }
    executor.setHost(nullptr);
  }

  CLOG->remove(logOutputter);
  delete logOutputter;
  return rc;
}

} // namespace deskflow
