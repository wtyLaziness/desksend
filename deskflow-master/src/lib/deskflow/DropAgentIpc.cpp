/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 DeskSend Contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "deskflow/DropAgentIpc.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <limits>

namespace deskflow {

namespace {

const char *commandTypeToString(DropAgentCommandType type)
{
  switch (type) {
  case DropAgentCommandType::UpdatePrimaryShape:
    return "update_primary_shape";
  case DropAgentCommandType::PrimaryMouseMove:
    return "primary_mouse_move";
  case DropAgentCommandType::PrimaryLeftMouseDown:
    return "primary_left_mouse_down";
  case DropAgentCommandType::LeftMouseUp:
    return "left_mouse_up";
  case DropAgentCommandType::HideSentinels:
    return "hide_sentinels";
  case DropAgentCommandType::ShutdownSentinels:
    return "shutdown_sentinels";
  case DropAgentCommandType::NudgeAcrossEdge:
    return "nudge_across_edge";
  case DropAgentCommandType::CancelNativeDrag:
    return "cancel_native_drag";
  case DropAgentCommandType::ShowHover:
    return "show_hover";
  case DropAgentCommandType::UpdateHoverCursor:
    return "update_hover_cursor";
  case DropAgentCommandType::HideHover:
    return "hide_hover";
  case DropAgentCommandType::BeginPreparing:
    return "begin_preparing";
  case DropAgentCommandType::CancelPreparing:
    return "cancel_preparing";
  case DropAgentCommandType::TransferBegin:
    return "transfer_begin";
  case DropAgentCommandType::TransferProgress:
    return "transfer_progress";
  case DropAgentCommandType::TransferFinish:
    return "transfer_finish";
  case DropAgentCommandType::StartReceiver:
    return "start_receiver";
  case DropAgentCommandType::StopReceiver:
    return "stop_receiver";
  case DropAgentCommandType::ReceiverAuthCommand:
    return "receiver_auth_command";
  case DropAgentCommandType::DropTarget:
    return "drop_target";
  case DropAgentCommandType::CancelDropToken:
    return "cancel_drop_token";
  case DropAgentCommandType::SendFiles:
    return "send_files";
  case DropAgentCommandType::CopyFilesToClipboard:
    return "copy_files_to_clipboard";
  case DropAgentCommandType::SetDragActive:
    return "set_drag_active";
  case DropAgentCommandType::Shutdown:
    return "shutdown";
  }
  return "shutdown";
}

std::optional<DropAgentCommandType> stringToCommandType(const QString &str)
{
  if (str == QLatin1String("update_primary_shape"))
    return DropAgentCommandType::UpdatePrimaryShape;
  if (str == QLatin1String("primary_mouse_move"))
    return DropAgentCommandType::PrimaryMouseMove;
  if (str == QLatin1String("primary_left_mouse_down"))
    return DropAgentCommandType::PrimaryLeftMouseDown;
  if (str == QLatin1String("left_mouse_up"))
    return DropAgentCommandType::LeftMouseUp;
  if (str == QLatin1String("hide_sentinels"))
    return DropAgentCommandType::HideSentinels;
  if (str == QLatin1String("shutdown_sentinels"))
    return DropAgentCommandType::ShutdownSentinels;
  if (str == QLatin1String("nudge_across_edge"))
    return DropAgentCommandType::NudgeAcrossEdge;
  if (str == QLatin1String("cancel_native_drag"))
    return DropAgentCommandType::CancelNativeDrag;
  if (str == QLatin1String("show_hover"))
    return DropAgentCommandType::ShowHover;
  if (str == QLatin1String("update_hover_cursor"))
    return DropAgentCommandType::UpdateHoverCursor;
  if (str == QLatin1String("hide_hover"))
    return DropAgentCommandType::HideHover;
  if (str == QLatin1String("begin_preparing"))
    return DropAgentCommandType::BeginPreparing;
  if (str == QLatin1String("cancel_preparing"))
    return DropAgentCommandType::CancelPreparing;
  if (str == QLatin1String("transfer_begin"))
    return DropAgentCommandType::TransferBegin;
  if (str == QLatin1String("transfer_progress"))
    return DropAgentCommandType::TransferProgress;
  if (str == QLatin1String("transfer_finish"))
    return DropAgentCommandType::TransferFinish;
  if (str == QLatin1String("start_receiver"))
    return DropAgentCommandType::StartReceiver;
  if (str == QLatin1String("stop_receiver"))
    return DropAgentCommandType::StopReceiver;
  if (str == QLatin1String("receiver_auth_command"))
    return DropAgentCommandType::ReceiverAuthCommand;
  if (str == QLatin1String("drop_target"))
    return DropAgentCommandType::DropTarget;
  if (str == QLatin1String("cancel_drop_token"))
    return DropAgentCommandType::CancelDropToken;
  if (str == QLatin1String("send_files"))
    return DropAgentCommandType::SendFiles;
  if (str == QLatin1String("copy_files_to_clipboard"))
    return DropAgentCommandType::CopyFilesToClipboard;
  if (str == QLatin1String("set_drag_active"))
    return DropAgentCommandType::SetDragActive;
  if (str == QLatin1String("shutdown"))
    return DropAgentCommandType::Shutdown;
  return std::nullopt;
}

const char *eventTypeToString(DropAgentEventType type)
{
  switch (type) {
  case DropAgentEventType::Ready:
    return "ready";
  case DropAgentEventType::PendingDragCaptured:
    return "pending_drag_captured";
  case DropAgentEventType::DragCancellationCompleted:
    return "drag_cancellation_completed";
  case DropAgentEventType::PendingDragCleared:
    return "pending_drag_cleared";
  case DropAgentEventType::ActiveTransfersChanged:
    return "active_transfers_changed";
  case DropAgentEventType::SendCompleted:
    return "send_completed";
  case DropAgentEventType::LogMessage:
    return "log_message";
  }
  return "ready";
}

std::optional<DropAgentEventType> stringToEventType(const QString &str)
{
  if (str == QLatin1String("ready"))
    return DropAgentEventType::Ready;
  if (str == QLatin1String("pending_drag_captured"))
    return DropAgentEventType::PendingDragCaptured;
  if (str == QLatin1String("drag_cancellation_completed"))
    return DropAgentEventType::DragCancellationCompleted;
  if (str == QLatin1String("pending_drag_cleared"))
    return DropAgentEventType::PendingDragCleared;
  if (str == QLatin1String("active_transfers_changed"))
    return DropAgentEventType::ActiveTransfersChanged;
  if (str == QLatin1String("send_completed"))
    return DropAgentEventType::SendCompleted;
  if (str == QLatin1String("log_message"))
    return DropAgentEventType::LogMessage;
  return std::nullopt;
}

QJsonArray toJsonStringArray(const std::vector<std::string> &items)
{
  QJsonArray arr;
  for (const auto &s : items) {
    arr.append(QString::fromStdString(s));
  }
  return arr;
}

std::vector<std::string> fromJsonStringArray(const QJsonArray &arr)
{
  std::vector<std::string> out;
  out.reserve(static_cast<size_t>(arr.size()));
  for (const QJsonValue &val : arr) {
    if (val.isString()) {
      out.push_back(val.toString().toStdString());
    }
  }
  return out;
}

bool readUint64(const QJsonValue &val, uint64_t &out)
{
  if (val.isString()) {
    bool ok = false;
    const qulonglong parsed = val.toString().toULongLong(&ok);
    if (!ok) {
      return false;
    }
    out = static_cast<uint64_t>(parsed);
    return true;
  }
  if (val.isDouble()) {
    const qint64 signedVal = val.toInteger(-1);
    if (signedVal < 0) {
      return false;
    }
    out = static_cast<uint64_t>(signedVal);
    return true;
  }
  return false;
}

bool readInt32(const QJsonValue &val, int32_t &out)
{
  if (!val.isDouble()) {
    return false;
  }
  const qint64 v = val.toInteger();
  if (v < std::numeric_limits<int32_t>::min() || v > std::numeric_limits<int32_t>::max()) {
    return false;
  }
  out = static_cast<int32_t>(v);
  return true;
}

} // namespace

std::string serializeDropAgentCommand(const DropAgentCommand &cmd)
{
  QJsonObject obj;
  obj.insert(QStringLiteral("type"), QString::fromLatin1(commandTypeToString(cmd.type)));
  if (cmd.x != 0)
    obj.insert(QStringLiteral("x"), static_cast<qint64>(cmd.x));
  if (cmd.y != 0)
    obj.insert(QStringLiteral("y"), static_cast<qint64>(cmd.y));
  if (cmd.ax != 0)
    obj.insert(QStringLiteral("ax"), static_cast<qint64>(cmd.ax));
  if (cmd.ay != 0)
    obj.insert(QStringLiteral("ay"), static_cast<qint64>(cmd.ay));
  if (cmd.aw != 0)
    obj.insert(QStringLiteral("aw"), static_cast<qint64>(cmd.aw));
  if (cmd.ah != 0)
    obj.insert(QStringLiteral("ah"), static_cast<qint64>(cmd.ah));
  if (cmd.activeSides != 0)
    obj.insert(QStringLiteral("activeSides"), static_cast<qint64>(cmd.activeSides));
  if (!cmd.edgeSegments.empty()) {
    QJsonArray segArr;
    for (const auto &seg : cmd.edgeSegments) {
      QJsonObject segObj;
      segObj.insert(QStringLiteral("side"), static_cast<int>(seg.side));
      segObj.insert(QStringLiteral("start"), static_cast<qint64>(seg.start));
      segObj.insert(QStringLiteral("end"), static_cast<qint64>(seg.end));
      segArr.append(segObj);
    }
    obj.insert(QStringLiteral("edgeSegments"), segArr);
  }
  if (cmd.flag)
    obj.insert(QStringLiteral("flag"), true);
  if (cmd.sideIndex != 0)
    obj.insert(QStringLiteral("sideIndex"), cmd.sideIndex);
  if (cmd.fileCount != 0)
    obj.insert(QStringLiteral("fileCount"), static_cast<qint64>(cmd.fileCount));
  if (cmd.done != 0)
    obj.insert(QStringLiteral("done"), QString::number(static_cast<qulonglong>(cmd.done)));
  if (cmd.total != 0)
    obj.insert(QStringLiteral("total"), QString::number(static_cast<qulonglong>(cmd.total)));
  if (cmd.dragActive)
    obj.insert(QStringLiteral("dragActive"), true);
  if (cmd.success)
    obj.insert(QStringLiteral("success"), true);
  if (!cmd.text.empty())
    obj.insert(QStringLiteral("text"), QString::fromStdString(cmd.text));
  if (!cmd.authToken.empty())
    obj.insert(QStringLiteral("authToken"), QString::fromStdString(cmd.authToken));
  if (!cmd.items.empty())
    obj.insert(QStringLiteral("items"), toJsonStringArray(cmd.items));

  return QJsonDocument(obj).toJson(QJsonDocument::Compact).toStdString();
}

std::optional<DropAgentCommand> deserializeDropAgentCommand(const std::string &line)
{
  if (line.size() > kMaxDropAgentLineBytes)
    return std::nullopt;
  const QByteArray raw = QByteArray::fromStdString(line).trimmed();
  if (raw.isEmpty() || !raw.startsWith('{')) {
    return std::nullopt;
  }
  QJsonParseError err{};
  const QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
  if (err.error != QJsonParseError::NoError || !doc.isObject()) {
    return std::nullopt;
  }
  const QJsonObject obj = doc.object();
  const auto typeOpt = stringToCommandType(obj.value(QStringLiteral("type")).toString());
  if (!typeOpt.has_value()) {
    return std::nullopt;
  }

  DropAgentCommand cmd;
  cmd.type = *typeOpt;
  if (obj.contains(QStringLiteral("x")) && !readInt32(obj.value(QStringLiteral("x")), cmd.x))
    return std::nullopt;
  if (obj.contains(QStringLiteral("y")) && !readInt32(obj.value(QStringLiteral("y")), cmd.y))
    return std::nullopt;
  if (obj.contains(QStringLiteral("ax")) && !readInt32(obj.value(QStringLiteral("ax")), cmd.ax))
    return std::nullopt;
  if (obj.contains(QStringLiteral("ay")) && !readInt32(obj.value(QStringLiteral("ay")), cmd.ay))
    return std::nullopt;
  if (obj.contains(QStringLiteral("aw")) && !readInt32(obj.value(QStringLiteral("aw")), cmd.aw))
    return std::nullopt;
  if (obj.contains(QStringLiteral("ah")) && !readInt32(obj.value(QStringLiteral("ah")), cmd.ah))
    return std::nullopt;
  if (obj.contains(QStringLiteral("activeSides"))) {
    const qint64 v = obj.value(QStringLiteral("activeSides")).toInteger(-1);
    if (v < 0 || v > std::numeric_limits<uint32_t>::max())
      return std::nullopt;
    cmd.activeSides = static_cast<uint32_t>(v);
  }
  if (obj.contains(QStringLiteral("edgeSegments"))) {
    const QJsonValue segVal = obj.value(QStringLiteral("edgeSegments"));
    if (!segVal.isArray())
      return std::nullopt;
    const QJsonArray arr = segVal.toArray();
    for (const QJsonValue &el : arr) {
      if (!el.isObject())
        return std::nullopt;
      const QJsonObject segObj = el.toObject();
      int32_t sideRaw = -1;
      int32_t start = 0;
      int32_t end = 0;
      if (!readInt32(segObj.value(QStringLiteral("side")), sideRaw) || sideRaw < 0 || sideRaw > 3 ||
          !readInt32(segObj.value(QStringLiteral("start")), start) ||
          !readInt32(segObj.value(QStringLiteral("end")), end)) {
        return std::nullopt;
      }
      cmd.edgeSegments.push_back(DragEdgeSegment{static_cast<DragEdgeSide>(sideRaw), start, end});
    }
  }
  if (obj.contains(QStringLiteral("flag")))
    cmd.flag = obj.value(QStringLiteral("flag")).toBool(false);
  if (obj.contains(QStringLiteral("sideIndex")))
    cmd.sideIndex = static_cast<int>(obj.value(QStringLiteral("sideIndex")).toInteger(0));
  if (obj.contains(QStringLiteral("fileCount"))) {
    const qint64 v = obj.value(QStringLiteral("fileCount")).toInteger(-1);
    if (v < 0 || v > std::numeric_limits<uint32_t>::max())
      return std::nullopt;
    cmd.fileCount = static_cast<uint32_t>(v);
  }
  if (obj.contains(QStringLiteral("done")) && !readUint64(obj.value(QStringLiteral("done")), cmd.done))
    return std::nullopt;
  if (obj.contains(QStringLiteral("total")) && !readUint64(obj.value(QStringLiteral("total")), cmd.total))
    return std::nullopt;
  if (obj.contains(QStringLiteral("dragActive")))
    cmd.dragActive = obj.value(QStringLiteral("dragActive")).toBool(false);
  if (obj.contains(QStringLiteral("success")))
    cmd.success = obj.value(QStringLiteral("success")).toBool(false);
  if (obj.contains(QStringLiteral("text")))
    cmd.text = obj.value(QStringLiteral("text")).toString().toStdString();
  if (obj.contains(QStringLiteral("authToken")))
    cmd.authToken = obj.value(QStringLiteral("authToken")).toString().toStdString();
  if (obj.contains(QStringLiteral("items"))) {
    const QJsonValue itemsVal = obj.value(QStringLiteral("items"));
    if (!itemsVal.isArray())
      return std::nullopt;
    cmd.items = fromJsonStringArray(itemsVal.toArray());
  }

  return cmd;
}

std::string serializeDropAgentEvent(const DropAgentEvent &ev)
{
  QJsonObject obj;
  obj.insert(QStringLiteral("type"), QString::fromLatin1(eventTypeToString(ev.type)));
  if (!ev.files.empty())
    obj.insert(QStringLiteral("files"), toJsonStringArray(ev.files));
  if (ev.sideIndex != 0)
    obj.insert(QStringLiteral("sideIndex"), ev.sideIndex);
  if (ev.x != 0)
    obj.insert(QStringLiteral("x"), static_cast<qint64>(ev.x));
  if (ev.y != 0)
    obj.insert(QStringLiteral("y"), static_cast<qint64>(ev.y));
  if (ev.flag)
    obj.insert(QStringLiteral("flag"), true);
  if (ev.level != 0)
    obj.insert(QStringLiteral("level"), ev.level);
  if (!ev.text.empty())
    obj.insert(QStringLiteral("text"), QString::fromStdString(ev.text));

  return QJsonDocument(obj).toJson(QJsonDocument::Compact).toStdString();
}

std::optional<DropAgentEvent> deserializeDropAgentEvent(const std::string &line)
{
  if (line.size() > kMaxDropAgentLineBytes)
    return std::nullopt;
  const QByteArray raw = QByteArray::fromStdString(line).trimmed();
  if (raw.isEmpty() || !raw.startsWith('{')) {
    return std::nullopt;
  }
  QJsonParseError err{};
  const QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
  if (err.error != QJsonParseError::NoError || !doc.isObject()) {
    return std::nullopt;
  }
  const QJsonObject obj = doc.object();
  const auto typeOpt = stringToEventType(obj.value(QStringLiteral("type")).toString());
  if (!typeOpt.has_value()) {
    return std::nullopt;
  }

  DropAgentEvent ev;
  ev.type = *typeOpt;
  if (obj.contains(QStringLiteral("files"))) {
    const QJsonValue filesVal = obj.value(QStringLiteral("files"));
    if (!filesVal.isArray())
      return std::nullopt;
    ev.files = fromJsonStringArray(filesVal.toArray());
  }
  if (obj.contains(QStringLiteral("sideIndex")))
    ev.sideIndex = static_cast<int>(obj.value(QStringLiteral("sideIndex")).toInteger(0));
  if (obj.contains(QStringLiteral("x")) && !readInt32(obj.value(QStringLiteral("x")), ev.x))
    return std::nullopt;
  if (obj.contains(QStringLiteral("y")) && !readInt32(obj.value(QStringLiteral("y")), ev.y))
    return std::nullopt;
  if (obj.contains(QStringLiteral("flag")))
    ev.flag = obj.value(QStringLiteral("flag")).toBool(false);
  if (obj.contains(QStringLiteral("level")))
    ev.level = static_cast<int>(obj.value(QStringLiteral("level")).toInteger(0));
  if (obj.contains(QStringLiteral("text")))
    ev.text = obj.value(QStringLiteral("text")).toString().toStdString();

  return ev;
}

} // namespace deskflow
