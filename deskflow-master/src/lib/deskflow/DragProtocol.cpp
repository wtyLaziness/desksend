/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 DeskSend Contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "deskflow/DragProtocol.h"

#include <algorithm>
#include <charconv>

namespace deskflow {

namespace {

constexpr std::string_view kEdgesPrefix = "DESKSEND_EDGES:";
constexpr std::string_view kPeersPrefix = "DESKSEND_PEERS:";
constexpr std::string_view kClientPreCapturePrefix = "DESKSEND_CLIENT_PRECAPTURE:";
constexpr std::string_view kHoverEnterPrefix = "DESKSEND_HOVER_ENTER:";
constexpr std::string_view kStartPrefix = "DESKSEND_START:";
constexpr std::string_view kDropPrefix = "DESKSEND_DROP:";
constexpr std::string_view kCancelExact = "DESKSEND_CANCEL";
constexpr std::string_view kCancelPrefix = "DESKSEND_CANCEL:";
constexpr std::string_view kSendDropPrefix = "DESKSEND_SEND_DROP:";
constexpr std::string_view kSendCancelExact = "DESKSEND_SEND_CANCEL";
constexpr std::string_view kSendCancelPrefix = "DESKSEND_SEND_CANCEL:";

bool parseInt32(std::string_view s, int32_t &out)
{
  if (s.empty()) {
    return false;
  }
  int32_t val = 0;
  const char *begin = s.data();
  const char *end = begin + s.size();
  const auto [ptr, ec] = std::from_chars(begin, end, val);
  if (ec != std::errc{} || ptr != end) {
    return false;
  }
  out = val;
  return true;
}

bool parseCoords(std::string_view s, int32_t &outX, int32_t &outY)
{
  const size_t comma = s.find(',');
  if (comma == std::string_view::npos) {
    return false;
  }
  int32_t x = 0;
  int32_t y = 0;
  if (!parseInt32(s.substr(0, comma), x) || !parseInt32(s.substr(comma + 1), y)) {
    return false;
  }
  outX = x;
  outY = y;
  return true;
}

bool startsWith(std::string_view s, std::string_view prefix)
{
  return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

} // namespace

char dragEdgeSideCode(DragEdgeSide side)
{
  switch (side) {
  case DragEdgeSide::Left:
    return 'L';
  case DragEdgeSide::Right:
    return 'R';
  case DragEdgeSide::Top:
    return 'T';
  case DragEdgeSide::Bottom:
    return 'B';
  }
  return 'L';
}

bool parseDragEdgeSideCode(char ch, DragEdgeSide &outSide)
{
  switch (ch) {
  case 'L':
    outSide = DragEdgeSide::Left;
    return true;
  case 'R':
    outSide = DragEdgeSide::Right;
    return true;
  case 'T':
    outSide = DragEdgeSide::Top;
    return true;
  case 'B':
    outSide = DragEdgeSide::Bottom;
    return true;
  default:
    return false;
  }
}

DirectionMask dragEdgeSideToDirectionMask(DragEdgeSide side)
{
  switch (side) {
  case DragEdgeSide::Left:
    return DirectionMask::LeftMask;
  case DragEdgeSide::Right:
    return DirectionMask::RightMask;
  case DragEdgeSide::Top:
    return DirectionMask::TopMask;
  case DragEdgeSide::Bottom:
    return DirectionMask::BottomMask;
  }
  return DirectionMask::NoDirMask;
}

std::optional<DragEdgeSide> directionToDragEdgeSide(Direction dir)
{
  switch (dir) {
  case Direction::Left:
    return DragEdgeSide::Left;
  case Direction::Right:
    return DragEdgeSide::Right;
  case Direction::Top:
    return DragEdgeSide::Top;
  case Direction::Bottom:
    return DragEdgeSide::Bottom;
  default:
    return std::nullopt;
  }
}

std::vector<DragEdgeSegment> clipEdgeSegmentsForCorners(
    const std::vector<DragEdgeSegment> &segments, int32_t screenWidth, int32_t screenHeight, int32_t cornerMargin
)
{
  std::vector<DragEdgeSegment> result;
  if (screenWidth <= 0 || screenHeight <= 0) {
    return result;
  }
  const int32_t margin = std::max<int32_t>(0, cornerMargin);
  for (const auto &seg : segments) {
    const int32_t extent =
        (seg.side == DragEdgeSide::Left || seg.side == DragEdgeSide::Right) ? screenHeight : screenWidth;
    if (extent <= 2 * margin) {
      continue;
    }
    const int32_t lo = std::max(seg.start, margin);
    const int32_t hi = std::min(seg.end, extent - margin);
    if (lo < hi) {
      result.push_back({seg.side, lo, hi});
    }
  }
  return result;
}

std::string serializeDragEdgesMessage(const std::vector<DragEdgeSegment> &segments)
{
  std::string out(kEdgesPrefix);
  for (size_t i = 0; i < segments.size(); ++i) {
    if (i > 0) {
      out.push_back(',');
    }
    out.push_back(dragEdgeSideCode(segments[i].side));
    out.push_back(':');
    out.append(std::to_string(segments[i].start));
    out.push_back('-');
    out.append(std::to_string(segments[i].end));
  }
  return out;
}

bool parseDragEdgesMessage(std::string_view info, std::vector<DragEdgeSegment> &outSegments)
{
  if (!startsWith(info, kEdgesPrefix)) {
    return false;
  }
  const std::string_view body = info.substr(kEdgesPrefix.size());
  std::vector<DragEdgeSegment> parsed;
  if (body.empty()) {
    outSegments.clear();
    return true;
  }
  size_t pos = 0;
  while (pos <= body.size()) {
    const size_t comma = body.find(',', pos);
    const std::string_view item =
        (comma == std::string_view::npos) ? body.substr(pos) : body.substr(pos, comma - pos);
    if (item.size() < 4 || item[1] != ':') {
      return false;
    }
    DragEdgeSide side = DragEdgeSide::Left;
    if (!parseDragEdgeSideCode(item[0], side)) {
      return false;
    }
    const std::string_view range = item.substr(2);
    const size_t dash = range.find('-', 1);
    if (dash == std::string_view::npos) {
      return false;
    }
    int32_t start = 0;
    int32_t end = 0;
    if (!parseInt32(range.substr(0, dash), start) || !parseInt32(range.substr(dash + 1), end) || start >= end) {
      return false;
    }
    parsed.push_back({side, start, end});
    if (comma == std::string_view::npos) {
      break;
    }
    pos = comma + 1;
  }
  outSegments = std::move(parsed);
  return true;
}

std::string serializeDragPeersMessage(const std::vector<std::string> &peers)
{
  std::string out(kPeersPrefix);
  bool first = true;
  for (const auto &peer : peers) {
    if (peer.empty()) {
      continue;
    }
    if (!first) {
      out.push_back(',');
    }
    first = false;
    out.append(peer);
  }
  return out;
}

bool parseDragPeersMessage(std::string_view info, std::vector<std::string> &outPeers)
{
  if (!startsWith(info, kPeersPrefix)) {
    return false;
  }
  const std::string_view body = info.substr(kPeersPrefix.size());
  std::vector<std::string> parsed;
  if (body.empty()) {
    outPeers.clear();
    return true;
  }
  size_t pos = 0;
  while (pos <= body.size()) {
    const size_t comma = body.find(',', pos);
    const std::string_view item =
        (comma == std::string_view::npos) ? body.substr(pos) : body.substr(pos, comma - pos);
    if (item.empty()) {
      return false;
    }
    parsed.emplace_back(item);
    if (comma == std::string_view::npos) {
      break;
    }
    pos = comma + 1;
  }
  outPeers = std::move(parsed);
  return true;
}

std::string serializeClientPreCaptureMessage(DragEdgeSide side, int32_t x, int32_t y)
{
  std::string out(kClientPreCapturePrefix);
  out.push_back(dragEdgeSideCode(side));
  out.push_back(':');
  out.append(std::to_string(x));
  out.push_back(',');
  out.append(std::to_string(y));
  return out;
}

bool parseClientPreCaptureMessage(std::string_view info, DragEdgeSide &outSide, int32_t &outX, int32_t &outY)
{
  if (!startsWith(info, kClientPreCapturePrefix)) {
    return false;
  }
  const std::string_view body = info.substr(kClientPreCapturePrefix.size());
  if (body.size() < 4 || body[1] != ':') {
    return false;
  }
  DragEdgeSide side = DragEdgeSide::Left;
  if (!parseDragEdgeSideCode(body[0], side)) {
    return false;
  }
  int32_t x = 0;
  int32_t y = 0;
  if (!parseCoords(body.substr(2), x, y)) {
    return false;
  }
  outSide = side;
  outX = x;
  outY = y;
  return true;
}

bool isClientPreCaptureClearMessage(std::string_view info)
{
  return info == kMsgDragClientPreCaptureClear;
}

bool isCommitHandoffMessage(std::string_view info)
{
  return info == kMsgDragCommitHandoff;
}

std::string serializeHoverEnterMessage(int32_t x, int32_t y)
{
  return std::string(kHoverEnterPrefix) + std::to_string(x) + "," + std::to_string(y);
}

bool parseHoverEnterMessage(std::string_view info, int32_t &outX, int32_t &outY)
{
  if (!startsWith(info, kHoverEnterPrefix)) {
    return false;
  }
  return parseCoords(info.substr(kHoverEnterPrefix.size()), outX, outY);
}

bool isHoverLeaveMessage(std::string_view info)
{
  return info == kMsgDragHoverLeave;
}

std::string serializeDragStartMessage(std::string_view token)
{
  std::string out(kStartPrefix);
  out.append(token);
  return out;
}

bool parseDragStartMessage(std::string_view info, std::string &outToken)
{
  if (!startsWith(info, kStartPrefix)) {
    return false;
  }
  std::string_view token = info.substr(kStartPrefix.size());
  const size_t lineEnd = token.find_first_of("\r\n");
  if (lineEnd != std::string_view::npos) {
    token = token.substr(0, lineEnd);
  }
  if (token.empty() || token.find_first_of(" \t") != std::string_view::npos) {
    return false;
  }
  outToken.assign(token);
  return true;
}

std::string serializeDragDropMessage(int32_t x, int32_t y, std::string_view tag)
{
  std::string out(kDropPrefix);
  out.append(std::to_string(x));
  out.push_back(',');
  out.append(std::to_string(y));
  if (!tag.empty()) {
    out.push_back(':');
    out.append(tag);
  }
  return out;
}

bool parseDragDropMessage(std::string_view info, int32_t &outX, int32_t &outY, std::string &outTag)
{
  if (!startsWith(info, kDropPrefix)) {
    return false;
  }
  const std::string_view body = info.substr(kDropPrefix.size());
  const size_t colon = body.find(':');
  const std::string_view coords = (colon == std::string_view::npos) ? body : body.substr(0, colon);
  if (!parseCoords(coords, outX, outY)) {
    return false;
  }
  if (colon == std::string_view::npos) {
    outTag.clear();
  } else {
    outTag.assign(body.substr(colon + 1));
  }
  return true;
}

std::string serializeDragCancelMessage(std::string_view token)
{
  if (token.empty()) {
    return std::string(kCancelExact);
  }
  std::string out(kCancelPrefix);
  out.append(token);
  return out;
}

bool parseDragCancelMessage(std::string_view info, std::string &outToken)
{
  if (info == kCancelExact) {
    outToken.clear();
    return true;
  }
  if (startsWith(info, kCancelPrefix)) {
    outToken.assign(info.substr(kCancelPrefix.size()));
    return true;
  }
  return false;
}

std::string serializeSendDropMessage(int32_t x, int32_t y, std::string_view token, std::string_view targetIp)
{
  std::string out(kSendDropPrefix);
  out.append(std::to_string(x));
  out.push_back(',');
  out.append(std::to_string(y));
  out.push_back(':');
  out.append(token);
  out.push_back(':');
  out.append(targetIp);
  return out;
}

bool parseSendDropMessage(
    std::string_view info, int32_t &outX, int32_t &outY, std::string &outToken, std::string &outTargetIp
)
{
  if (!startsWith(info, kSendDropPrefix)) {
    return false;
  }
  const std::string_view body = info.substr(kSendDropPrefix.size());
  // 只按前两个冒号切分，避免截断含多个冒号的 IPv6 目标地址。
  const size_t firstColon = body.find(':');
  if (firstColon == std::string_view::npos) {
    return false;
  }
  const size_t secondColon = body.find(':', firstColon + 1);
  if (secondColon == std::string_view::npos) {
    return false;
  }
  const std::string_view coords = body.substr(0, firstColon);
  const std::string_view token = body.substr(firstColon + 1, secondColon - firstColon - 1);
  const std::string_view target = body.substr(secondColon + 1);
  if (token.empty() || target.empty() || !parseCoords(coords, outX, outY)) {
    return false;
  }
  outToken.assign(token);
  outTargetIp.assign(target);
  return true;
}

std::string serializeSendCancelMessage(std::string_view token)
{
  if (token.empty()) {
    return std::string(kSendCancelExact);
  }
  std::string out(kSendCancelPrefix);
  out.append(token);
  return out;
}

bool parseSendCancelMessage(std::string_view info, std::string &outToken)
{
  if (info == kSendCancelExact) {
    outToken.clear();
    return true;
  }
  if (startsWith(info, kSendCancelPrefix)) {
    outToken.assign(info.substr(kSendCancelPrefix.size()));
    return true;
  }
  return false;
}

namespace {

constexpr std::string_view kTargetsPrefix = "DESKSEND_TRANSFER_TARGETS:";
constexpr std::string_view kDirectRequestPrefix = "DESKSEND_DIRECT_SEND_REQ:";
constexpr std::string_view kDirectGrantPrefix = "DESKSEND_DIRECT_SEND_GRANT:";
constexpr std::string_view kDirectCancelPrefix = "DESKSEND_DIRECT_SEND_CANCEL:";

bool validDirectField(std::string_view value)
{
  return value.find_first_of(std::string_view("\r\n\0", 3)) == std::string_view::npos;
}

bool validDirectId(std::string_view value)
{
  return !value.empty() && validDirectField(value) && value.find(':') == std::string_view::npos;
}

} // namespace

std::string serializeTransferTargetsMessage(const std::vector<std::string> &targets)
{
  std::string message(kTargetsPrefix);
  for (const auto &target : targets) {
    if (!validDirectField(target) || target.find(',') != std::string::npos) {
      return {};
    }
    if (!target.empty()) {
      if (message.size() > kTargetsPrefix.size()) {
        message += ',';
      }
      message += target;
    }
  }
  return message;
}

bool parseTransferTargetsMessage(std::string_view info, std::vector<std::string> &outTargets)
{
  if (!startsWith(info, kTargetsPrefix) || !validDirectField(info)) {
    return false;
  }
  auto body = info.substr(kTargetsPrefix.size());
  std::vector<std::string> targets;
  while (!body.empty()) {
    const auto comma = body.find(',');
    const auto name = body.substr(0, comma);
    if (name.empty() || comma == body.size() - 1) {
      return false;
    }
    targets.emplace_back(name);
    if (comma == std::string_view::npos) {
      break;
    }
    body.remove_prefix(comma + 1);
  }
  outTargets = std::move(targets);
  return true;
}

std::string serializeDirectSendRequestMessage(std::string_view requestId, std::string_view targetPeer)
{
  if (!validDirectId(requestId) || !validDirectField(targetPeer)) {
    return {};
  }
  return std::string(kDirectRequestPrefix).append(requestId).append(":").append(targetPeer);
}

bool parseDirectSendRequestMessage(std::string_view info, std::string &outRequestId, std::string &outTargetPeer)
{
  if (!startsWith(info, kDirectRequestPrefix) || !validDirectField(info)) {
    return false;
  }
  const auto body = info.substr(kDirectRequestPrefix.size());
  const auto colon = body.find(':');
  if (colon == 0 || colon == std::string_view::npos) {
    return false;
  }
  outRequestId.assign(body.substr(0, colon));
  outTargetPeer.assign(body.substr(colon + 1));
  return true;
}

std::string serializeDirectSendGrantMessage(std::string_view requestId, std::string_view token, std::string_view targetIp)
{
  if (!validDirectId(requestId) || !validDirectId(token) || targetIp.empty() || !validDirectField(targetIp)) {
    return {};
  }
  return std::string(kDirectGrantPrefix).append(requestId).append(":").append(token).append(":").append(targetIp);
}

bool parseDirectSendGrantMessage(
    std::string_view info, std::string &outRequestId, std::string &outToken, std::string &outTargetIp
)
{
  if (!startsWith(info, kDirectGrantPrefix) || !validDirectField(info)) {
    return false;
  }
  const auto body = info.substr(kDirectGrantPrefix.size());
  const auto first = body.find(':');
  if (first == 0 || first == std::string_view::npos) {
    return false;
  }
  const auto second = body.find(':', first + 1);
  if (second == std::string_view::npos || second == first + 1 || second == body.size() - 1) {
    return false;
  }
  outRequestId.assign(body.substr(0, first));
  outToken.assign(body.substr(first + 1, second - first - 1));
  outTargetIp.assign(body.substr(second + 1));
  return true;
}

std::string serializeDirectSendCancelMessage(std::string_view requestId)
{
  return validDirectId(requestId) ? std::string(kDirectCancelPrefix).append(requestId) : std::string{};
}

bool parseDirectSendCancelMessage(std::string_view info, std::string &outRequestId)
{
  if (!startsWith(info, kDirectCancelPrefix) || !validDirectId(info.substr(kDirectCancelPrefix.size()))) {
    return false;
  }
  outRequestId.assign(info.substr(kDirectCancelPrefix.size()));
  return true;
}

std::string redactDragLogPayload(std::string_view info)
{
  if (startsWith(info, kDirectGrantPrefix)) {
    std::string requestId;
    std::string token;
    std::string targetIp;
    if (parseDirectSendGrantMessage(info, requestId, token, targetIp)) {
      return serializeDirectSendGrantMessage(requestId, "<redacted>", targetIp);
    }
    return std::string(kDirectGrantPrefix) + "<redacted>";
  }
  if (startsWith(info, "DESKSEND_START")) {
    return "DESKSEND_START:<redacted>";
  }
  if (startsWith(info, kCancelPrefix)) {
    return "DESKSEND_CANCEL:<redacted>";
  }
  if (startsWith(info, kSendCancelPrefix)) {
    return "DESKSEND_SEND_CANCEL:<redacted>";
  }
  if (startsWith(info, kSendDropPrefix)) {
    const std::string_view body = info.substr(kSendDropPrefix.size());
    const size_t firstColon = body.find(':');
    if (firstColon != std::string_view::npos) {
      const size_t secondColon = body.find(':', firstColon + 1);
      if (secondColon != std::string_view::npos) {
        std::string out(kSendDropPrefix);
        out.append(body.substr(0, firstColon));
        out.append(":<redacted>:");
        out.append(body.substr(secondColon + 1));
        return out;
      }
    }
    return "DESKSEND_SEND_DROP:<redacted>";
  }
  if (startsWith(info, kDropPrefix)) {
    const std::string_view body = info.substr(kDropPrefix.size());
    const size_t colon = body.find(':');
    if (colon != std::string_view::npos) {
      std::string out(kDropPrefix);
      out.append(body.substr(0, colon));
      out.append(":<redacted>");
      return out;
    }
  }
  return std::string(info);
}

} // namespace deskflow
