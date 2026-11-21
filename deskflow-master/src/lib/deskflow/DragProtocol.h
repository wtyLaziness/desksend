/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 DeskSend Contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "base/DirectionTypes.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace deskflow {

enum class DragEdgeSide : uint8_t
{
  Left = 0,
  Right = 1,
  Top = 2,
  Bottom = 3,
};

struct DragEdgeSegment
{
  DragEdgeSide side = DragEdgeSide::Left;
  int32_t start = 0; // Inclusive pixel offset along edge: [0, height) for Left/Right, [0, width) for Top/Bottom
  int32_t end = 0;   // Exclusive pixel offset along edge

  bool operator==(const DragEdgeSegment &) const = default;
};

inline constexpr std::string_view kMsgDragClientPreCaptureClear = "DESKSEND_CLIENT_PRECAPTURE_CLEAR";
inline constexpr std::string_view kMsgDragCommitHandoff = "DESKSEND_COMMIT_HANDOFF";
inline constexpr std::string_view kMsgDragHoverLeave = "DESKSEND_HOVER_LEAVE";
inline constexpr int32_t kDirectSendDropCoord = -100000;
inline constexpr std::string_view kServerPeerTargetMarker = "__SERVER__";

char dragEdgeSideCode(DragEdgeSide side);
bool parseDragEdgeSideCode(char ch, DragEdgeSide &outSide);
DirectionMask dragEdgeSideToDirectionMask(DragEdgeSide side);
std::optional<DragEdgeSide> directionToDragEdgeSide(Direction dir);

std::vector<DragEdgeSegment> clipEdgeSegmentsForCorners(
    const std::vector<DragEdgeSegment> &segments, int32_t screenWidth, int32_t screenHeight, int32_t cornerMargin = 16
);

std::string serializeDragEdgesMessage(const std::vector<DragEdgeSegment> &segments);
bool parseDragEdgesMessage(std::string_view info, std::vector<DragEdgeSegment> &outSegments);

std::string serializeDragPeersMessage(const std::vector<std::string> &peers);
bool parseDragPeersMessage(std::string_view info, std::vector<std::string> &outPeers);

std::string serializeClientPreCaptureMessage(DragEdgeSide side, int32_t x, int32_t y);
bool parseClientPreCaptureMessage(std::string_view info, DragEdgeSide &outSide, int32_t &outX, int32_t &outY);

bool isClientPreCaptureClearMessage(std::string_view info);
bool isCommitHandoffMessage(std::string_view info);

std::string serializeHoverEnterMessage(int32_t x, int32_t y);
bool parseHoverEnterMessage(std::string_view info, int32_t &outX, int32_t &outY);
bool isHoverLeaveMessage(std::string_view info);

std::string serializeDragStartMessage(std::string_view token);
bool parseDragStartMessage(std::string_view info, std::string &outToken);

std::string serializeDragDropMessage(int32_t x, int32_t y, std::string_view tag = {});
bool parseDragDropMessage(std::string_view info, int32_t &outX, int32_t &outY, std::string &outTag);

std::string serializeDragCancelMessage(std::string_view token = {});
bool parseDragCancelMessage(std::string_view info, std::string &outToken);

std::string serializeSendDropMessage(int32_t x, int32_t y, std::string_view token, std::string_view targetIp);
bool parseSendDropMessage(
    std::string_view info, int32_t &outX, int32_t &outY, std::string &outToken, std::string &outTargetIp
);

std::string serializeSendCancelMessage(std::string_view token = {});
bool parseSendCancelMessage(std::string_view info, std::string &outToken);

std::string serializeTransferTargetsMessage(const std::vector<std::string> &targets);
bool parseTransferTargetsMessage(std::string_view info, std::vector<std::string> &outTargets);
std::string serializeDirectSendRequestMessage(std::string_view requestId, std::string_view targetPeer);
bool parseDirectSendRequestMessage(std::string_view info, std::string &outRequestId, std::string &outTargetPeer);
std::string serializeDirectSendGrantMessage(std::string_view requestId, std::string_view token, std::string_view targetIp);
bool parseDirectSendGrantMessage(
    std::string_view info, std::string &outRequestId, std::string &outToken, std::string &outTargetIp
);
std::string serializeDirectSendCancelMessage(std::string_view requestId);
bool parseDirectSendCancelMessage(std::string_view info, std::string &outRequestId);

std::string redactDragLogPayload(std::string_view info);

} // namespace deskflow
