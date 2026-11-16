/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 DeskSend Contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/DragProtocol.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace deskflow {

enum class DropAgentCommandType
{
  UpdatePrimaryShape,
  PrimaryMouseMove,
  PrimaryLeftMouseDown,
  LeftMouseUp,
  HideSentinels,
  ShutdownSentinels,
  NudgeAcrossEdge,
  CancelNativeDrag,
  ShowHover,
  UpdateHoverCursor,
  HideHover,
  BeginPreparing,
  CancelPreparing,
  TransferBegin,
  TransferProgress,
  TransferFinish,
  StartReceiver,
  StopReceiver,
  ReceiverAuthCommand,
  DropTarget,
  CancelDropToken,
  SendFiles,
  CopyFilesToClipboard,
  SetDragActive,
  Shutdown
};

struct DropAgentCommand
{
  DropAgentCommandType type = DropAgentCommandType::Shutdown;
  int32_t x = 0;
  int32_t y = 0;
  int32_t ax = 0;
  int32_t ay = 0;
  int32_t aw = 0;
  int32_t ah = 0;
  uint32_t activeSides = 0;
  std::vector<DragEdgeSegment> edgeSegments;
  bool flag = false;
  int sideIndex = 0;
  uint32_t fileCount = 0;
  uint64_t done = 0;
  uint64_t total = 0;
  bool dragActive = false;
  bool success = false;
  std::string text;
  std::string authToken;
  std::vector<std::string> items;

  bool operator==(const DropAgentCommand &) const = default;
};

enum class DropAgentEventType
{
  Ready,
  PendingDragCaptured,
  DragCancellationCompleted,
  PendingDragCleared,
  ActiveTransfersChanged,
  SendCompleted,
  LogMessage
};

struct DropAgentEvent
{
  DropAgentEventType type = DropAgentEventType::Ready;
  std::vector<std::string> files;
  int sideIndex = 0;
  int32_t x = 0;
  int32_t y = 0;
  bool flag = false;
  int level = 0;
  std::string text;

  bool operator==(const DropAgentEvent &) const = default;
};

std::string serializeDropAgentCommand(const DropAgentCommand &cmd);
std::optional<DropAgentCommand> deserializeDropAgentCommand(const std::string &line);

std::string serializeDropAgentEvent(const DropAgentEvent &ev);
std::optional<DropAgentEvent> deserializeDropAgentEvent(const std::string &line);

} // namespace deskflow
