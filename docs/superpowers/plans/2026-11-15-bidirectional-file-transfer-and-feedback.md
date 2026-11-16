# 双向文件拖拽传输、接收端反馈与基础修复实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 DeskSend 实现“主机共享鼠标”下的对称双向（含三机串联无损多跳穿透）文件拖放、接收端两段式即时视觉反馈，原生兼容 Windows 安装版 `Service + Daemon::Elevate = true` 权限模型，并一并修复已定位的卡顿与拖拽基础缺陷。

**Architecture:** 先落定传输/定时器/Win32 输入热路径与拖拽基础修复，清理 `ClientProxy1_5::dragInfoReceived` 读流缺口及双向 `kMsgDDragInfo` 线段协议；再抽取 Qt 光标跟随胶囊与即时 `TransferStatus` 界面模块；随后将受 UIPI/用户路径限制的 OLE 哨兵、目录解析、`localsend-cli` 与浮窗抽到 `DropExecutor`（非提权同进程直调 / `SYSTEM` 下通过 `WTSQueryUserToken` + 继承匿名管道拉起同二进制 `--drop-agent`），最终在 `Server` / `Client` / `FileDropBridge` 合入统一 `ActiveCrossDrag` 二段确认与多跳状态机。

**Tech Stack:** C++20, Qt 6.8 (`QtWidgets` / `QtNetwork` / `QtTest`), Win32 API (`SendInput`, `SetWindowsHookEx`, OLE `IDropTarget`, `WTSQueryUserToken`, `CreateProcessAsUserW`, Job Object), Ninja + MSVC x64 + CMake / CTest via GitHub Actions (`.github/workflows/deskflow-win.yml`), `localsend-cli` (Rust).

**Spec:** [2026-11-15-bidirectional-file-transfer-and-feedback-design.md](file:///d:/Desktop/desksend/docs/superpowers/specs/2026-11-15-bidirectional-file-transfer-and-feedback-design.md)

## Global Constraints

- **Windows 默认安装权限（方案 X）**：支持 `Core::ProcessMode = Service` + `Daemon::Elevate = true`（`deskflow-core` 为 `NT AUTHORITY\SYSTEM` + `TokenUIAccess=1`）；OLE `RegisterDragDrop`、`FOLDERID_Desktop`、`CLSID_ShellWindows`、`QStandardPaths::DownloadLocation`、`TransferStatus`、`DragHoverIndicator` 与 `localsend-cli` 必须运行在非提权当前登录用户会话，跨进程仅允许继承匿名管道，禁止暴露公共命名管道；`VK_ESCAPE` 与局部 `LEFTUP` 保持在核心进程内执行以穿透高完整性目标窗口。
- **双端 LocalSend 端口与配置隔离**：接收端固定端口 `53318`（可通过 `DESKSEND_RECEIVER_PORT` 覆盖）+ `--auth-stdin` + 逗号分隔 `--auto-accept-from`；发送端 `--port 0` 动态分配，使用独立的 `sender` / `receiver` `XDG_CONFIG_HOME` 子目录。禁止在活动拖拽或传输过程中重启接收器。
- **协议向后兼容**：复用 `kMsgDDragInfo`（`"DDRG%2i%s"`），不改主次版本号；保持原有 `DESKSEND_START:<token>`、`DESKSEND_DROP:<x>,<y>[:<tag>]`、`DESKSEND_CANCEL[:<token>]` 解析兼容，并对含 token 的控制消息做日志脱敏。
- **反截断与防误取消**：普通非文件拖拽（非 `CF_HDROP`）不得拦截或触发 `VK_ESCAPE`；副机 `DragEnter` 采用服务端二段确认，仅收到 `DESKSEND_COMMIT_HANDOFF` 后才取消本地 Explorer 拖拽；跟随胶囊必须鼠标穿透（`WS_EX_TRANSPARENT | WS_EX_NOACTIVATE` + `WA_TransparentForMouseEvents` + `WA_ShowWithoutActivating`）。
- **验证条件**：本机无 `cmake`/`cl`/`ninja` 编译器，所有构建与全量 `CTest` 在提交推送至 GitHub 后由 `.github/workflows/deskflow-win.yml` 执行验收（每步单测命令亦给出精确 CTest 过滤器以便本地或具备 MSVC 工具链环境复现）。

## Review Focus

1. **普通窗口拖拽 / 非 `CF_HDROP` 与 `DragLeave` 防误伤**：在主副屏边缘拖动 Chrome 窗口或文本、或带文件碰边后未切屏即退回（`DragLeave`）时，`SentinelDropTarget` 必须返回 `DROPEFFECT_NONE`、不发 `VK_ESCAPE`、不在松手后遗留可见哨兵窗，测试需覆盖非 `CF_HDROP` 与 `PreCaptured -> DragLeave` 路径。
2. **上行 `kMsgDDragInfo` 协议流对齐**：`ClientProxy1_5::dragInfoReceived` 读完一条 `"DDRG"` 后，紧跟其后的 `kMsgCKeepAlive` / `kMsgCClipboard` 包不得因未消费字节或类型错位解析失败。
3. **活动传输期延后白名单重启**：正在执行 `localsend-cli` 接收或跨屏拖拽途中连接/断开第三台客户端触发 `DESKSEND_PEERS` 变更时，必须等会话结束后再重启接收器，不得踢掉当前授权 token。
4. **三机串联多跳与退回源屏状态完整性**：`[Client A] — [Primary] — [Client B]` 连续拖拽不松手穿越 `Primary` 到 `Client B` 时，必须清掉 `Primary` 胶囊且保持 `Client A` 待发文件并在 `Client B` 松手完成 `A -> B` 直传；中途拖回 `Client A` 时全链路清理回滚。
5. **`SYSTEM` 提权模式 `--drop-agent` 实例锁隔离与无用户会话兜底**：`deskflow-core.exe --drop-agent` 必须在 `QSharedMemory(kCoreBinName)` 和 `CoreIpcServer::listen()` 之前独立进入；当无活跃控制台用户（锁屏）导致获取用户令牌失败时，核心键鼠与服务流程不得崩溃或阻塞，并在用户登录后按需恢复拉起。

---

### Task 1: 卡顿与传输/定时器/Win32 坐标热点修复

**Files:**
- Modify: `deskflow-master/src/lib/deskflow/PacketStreamFilter.cpp:69-82`
- Modify: `deskflow-master/src/lib/client/ServerProxy.cpp:201-216`
- Modify: `deskflow-master/src/lib/server/ClientProxy1_0.h:64-101`
- Modify: `deskflow-master/src/lib/server/ClientProxy1_0.cpp:76-107,140-150`
- Modify: `deskflow-master/src/lib/server/ClientProxy1_3.cpp:63-93`
- Modify: `deskflow-master/src/lib/platform/MSWindowsComputer.cpp:1397-1448`
- Modify: `deskflow-master/src/lib/platform/MSWindowsDesks.h:252-259`
- Modify: `deskflow-master/src/lib/platform/MSWindowsDesks.cpp:456-507`
- Test: `deskflow-master/src/unittests/client/ServerProxyTests.cpp`
- Test: `deskflow-master/src/unittests/server/ClientProxyTests.cpp`
- Test: `deskflow-master/src/unittests/platform/MSWindowsDesksTests.cpp`
- Test: `deskflow-master/src/unittests/deskflow/ClipboardChunksTests.cpp` 或 `deskflow-master/src/unittests/deskflow/FileDropBridgeTests.cpp`（新增 `PacketStreamFilter` 单写验证，见下方 `PacketStreamFilterTests.cpp`）

**Interfaces:**
- Produces in `MSWindowsDesks`（`platform/MSWindowsDesks.h` 公有/友元可测静态函数）:
  `static std::pair<uint32_t, uint32_t> normalizeVirtualDesktopPoint(int32_t x, int32_t y, int32_t vx, int32_t vy, int32_t vw, int32_t vh);`

- [ ] **Step 1: 为 `PacketStreamFilter::write`、`ServerProxy::handleMessage`、`ClientProxy1_3::resetHeartbeatTimer` 与 `normalizeVirtualDesktopPoint` 写失败单测**

  - 在 `deskflow-master/src/unittests/deskflow/FileDropBridgeTests.cpp`（或独立 `PacketStreamFilterTests` 注册到 `unittests/deskflow/CMakeLists.txt`）添加测试 `packetStreamFilterWriteWritesSingleFrame()`：传入计数 `CapturingStream` 验证一次 `write("abcd", 4)` 仅触发 **1 次** `write()` 调用且内容为 `\x00\x00\x00\x04abcd`（以及大于栈缓冲 `> 4096` 字节大包路径）。
  - 在 `ServerProxyTests.cpp` 添加 `handleMessage_doesNotSendNoopPerMouseMove()`：推入一条 `kMsgDMouseMove(10, 20)`，断言输出流写入字节中**不包含** `kMsgCNoop`。
  - 在 `ClientProxyTests.cpp` 添加 `resetHeartbeatTimer_reusesTimersWithoutChurn()`：创建 `ClientProxy1_6` 配合记录 `newOneShotTimer` / `deleteTimer` 次数的 `MockEventQueue`，连续调用 50 次 `resetHeartbeatTimer()`，断言 `deleteTimer` 次数 <= 1 且定时器总数不变。
  - 在 `MSWindowsDesksTests.cpp` 添加 `normalizeVirtualDesktopPoint_handlesNegativeOriginMonitors()` 与 `defaultShapeYIsZero()`：验证 `(x=-960, y=540)` 在虚拟桌面 `(-1920, 0, 3840x1080)` 映射到 `(16384, 32768)`（区间 `[0, 65535]`）；以及新建 `MSWindowsDesks` 默认 `m_y == 0`（原为 `9`）。

- [ ] **Step 2: 运行测试确认失败（无本机编译器则在工具链环境/CI 校验基线红灯）**

  Run: `ctest --test-dir deskflow-master/build/src/unittests --output-on-failure -R "FileDropBridgeTests|ServerProxyTests|ClientProxyTests|MSWindowsDesksTests"`
  Expected: FAIL（`PacketStreamFilter` 调用 2 次 write、`ServerProxy` 回写 `DMOP`/`kMsgCNoop`、`ClientProxy` 产生 50 次 `deleteTimer`、`MSWindowsDesks` 缺符号且 `m_y == 9`）。

- [ ] **Step 3: 实现 `PacketStreamFilter::write` 合并写出与 `ServerProxy` 去 `kMsgCNoop` 回声**

  - 在 `PacketStreamFilter.cpp:69-81` 将 4 字节大端长度前缀与负载拷贝入单块连续缓冲（`count <= 4092` 用栈上 4096 字节缓冲，大包用 `std::vector<uint8_t>`）并只调用 1 次 `getStream()->write(...)`。
  - 在 `ServerProxy.cpp:201-215` 移除每次 `handleMessage` 返回 `Okay` 后无条件写入 `kMsgCNoop` 的逻辑（保留 `kMsgCKeepAlive` 的双向心跳响应与读取端对旧版 `kMsgCNoop` 的兼容丢弃）。

- [ ] **Step 4: 实现 `ClientProxy1_0` / `ClientProxy1_3` 无重建心跳节流**

  - 在 `ClientProxy1_0.h` 新增 `bool m_heartbeatDirty = false;`（或 `Stopwatch m_heartbeatWatch;`）及定时器事件处理：保持已有单一 `EventQueueTimer *m_heartbeatTimer` 按检查周期运行，收到数据包调用 `resetHeartbeatTimer()` 时只置位活跃时间/标记 `m_heartbeatDirty = true`（`O(1)` 原子/字段写入），仅当定时器真正到期且自上次置位后未满 `m_heartbeatAlarm` 时按剩余时间重新调度；仅 `setHeartbeatRate` / 析构时重建或删除定时器。`ClientProxy1_3::resetHeartbeatTimer()` 委托给该 `O(1)` 刷新而非调用 `removeHeartbeatTimer() + addHeartbeatTimer()`。

- [ ] **Step 5: 实现 `MSWindowsComputer::warpCursorNoFlush` 与 `MSWindowsDesks` 移动及虚拟桌面精确坐标修复**

  - 在 `MSWindowsComputer.cpp:1397-1448`：移除 `Arch::sleep(0.0)` 让出时间片；主屏幕无需额外回读 `GetCursorPos`（保留 `!isPrimary()` 时针对 Vista/Win7 登录屏的失败兜底）。
  - 在 `MSWindowsDesks.h:254` 将 `int32_t m_y = 9;` 修正为 `int32_t m_y = 0;`。
  - 在 `MSWindowsDesks.cpp` 新增 `MSWindowsDesks::normalizeVirtualDesktopPoint(...)`，用 `double` 按全虚拟桌面原宽高计算 `[0, 65535]` 归一化坐标，并在 `deskMouseMove(int32_t x, int32_t y)` 读取 `SM_XVIRTUALSCREEN` / `SM_YVIRTUALSCREEN` / `SM_CXVIRTUALSCREEN` / `SM_CYVIRTUALSCREEN` 传入 `MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK`（回退 `SetCursorPos`）。
  - 在 `MSWindowsDesks.cpp:470-506` 优化 `deskMouseRelativeMove(int32_t dx, int32_t dy)`：在进入/开启 1:1 相对模式时按会话/桌面周期最多同步一次或跳过每包 4 次阻塞式 `SystemParametersInfo` 调用（用本地偏移余数/单次 `send_mouse_input(MOUSEEVENTF_MOVE, dx, dy, 0)` 发送，避免在高频移动包路径内同步改全局系统参数）。

- [ ] **Step 6: 运行 Task 1 相关单测确认通过并提交**

  Run: `cmake --build deskflow-master/build && ctest --test-dir deskflow-master/build/src/unittests --output-on-failure -R "FileDropBridgeTests|ServerProxyTests|ClientProxyTests|MSWindowsDesksTests"`
  Expected: PASS（新增与现有用例全绿）
  ```bash
  git add deskflow-master/src/lib/deskflow/PacketStreamFilter.cpp deskflow-master/src/lib/client/ServerProxy.cpp deskflow-master/src/lib/server/ClientProxy1_0.h deskflow-master/src/lib/server/ClientProxy1_0.cpp deskflow-master/src/lib/server/ClientProxy1_3.cpp deskflow-master/src/lib/platform/MSWindowsComputer.cpp deskflow-master/src/lib/platform/MSWindowsDesks.h deskflow-master/src/lib/platform/MSWindowsDesks.cpp deskflow-master/src/unittests/
  git commit -m "perf: eliminate mouse/network stutter and fix virtual desktop cursor mapping"
  ```

---

### Task 2: 拖拽边缘几何、`Server.cpp` 偏移修正与 `MSWindowsHook` 基础清理

**Files:**
- Modify: `deskflow-master/src/lib/server/Server.cpp:406-429,880-920,1643-1657,1868-1870`
- Modify: `deskflow-master/src/lib/platform/MSWindowsHook.cpp:617-628`
- Modify: `deskflow-master/src/lib/deskflow/FileDropBridge.cpp:427-462,465-535,595-656,845-925`
- Test: `deskflow-master/src/unittests/server/ServerTests.cpp`
- Test: `deskflow-master/src/unittests/deskflow/FileDropBridgeTests.cpp`

**Interfaces:**
- Produces on `deskflow::FileDropBridge` (`FileDropBridge.h`):
  - `static bool isSentinelCaptureSupportedFormat(bool hasHdrop, bool nonEmpty);` (或集成在 `setPendingDragFiles` / `SentinelDropTarget::DragEnter` 过滤中)
  - `void clearPendingDragOnMouseLeaveOrRelease();`

- [ ] **Step 1: 为 `Server.cpp` 底边等待区域判断、非文件拖拽过滤及 `onLeftMouseUp(false)` 隐藏哨兵写失败单测**

  - 在 `ServerTests.cpp` 添加 `bottomEdgeSwitchWaitClearsInsideBottomZone()`：验证 `1920x1080` 且 `zoneSize=1` 时，`m_y = 1078`（位于底边界内侧，`< 1079`）能取消等待（原 `<= 1079 + 1` 错误地把边界像素 `1079` 自身当成内部而清掉等待）。
  - 在 `FileDropBridgeTests.cpp` 添加 `onLeftMouseUp_hidesSentinelAndClearsCancellationFlags()` 与 `nonFileDragDoesNotSetPendingFiles()`：验证传入空/非文件集合不进入 `hasPendingDragFiles()`；验证 `onLeftMouseUp(false)` 清理挂起文件并关闭哨兵可见态（对齐 **Review Focus #1**）。

- [ ] **Step 2: 验证测试基线失败**

  Run: `ctest --test-dir deskflow-master/build/src/unittests --output-on-failure -R "ServerTests|FileDropBridgeTests"`
  Expected: FAIL（`Server.cpp:1869` 使用 `+ zoneSize`、`FileDropBridge::onLeftMouseUp(false)` 未隐藏哨兵/清理 pending 取消状态）。

- [ ] **Step 3: 修正 `Server.cpp:1869`、`noSwitch` 竞态及 `MSWindowsHook.cpp` 边缘左键放行条件**

  - 在 `Server.cpp:1869` 将 `clearWait = (m_y <= ay + ah - 1 + zoneSize);` 修正为 `clearWait = (m_y <= ay + ah - 1 - zoneSize);`。
  - 在 `Server.cpp` 检查 `noSwitch`（约 `880-920` 行）确保仅当未进入活动 `m_pendingFiles` 取消等待前按正常逻辑中止；避免 `m_switchComputer` 因微小反向移动取消后留下卡死的 `cancellingNativeDrag` 标志。
  - 在 `MSWindowsHook.cpp:620-626` 修复 `VK_LBUTTON` 边缘透传：确保当光标处于配置边缘内侧（`inside && !bogus`）但左键按下时，仍正确向主线程投递夹逼位置通知，同时仅对正在触碰跳变带的外推运动返回受控透传以允许系统把 `WM_MOUSEMOVE`/OLE `DoDragDrop` 送达紧贴边界的 `DeskflowSentinel`。

- [ ] **Step 4: 对齐 `Win32SentinelManager` 尺寸与非文件放行、取消竞态清理**

  - 在 `FileDropBridge.cpp:467` 将 `kThickness` 由 `8` 调整为 `2`（紧贴 `1px` 切屏区避免提前 8px 截断后悬停死区），并缩小过宽触发带 `kActivationZone` 到 `16`（仅在靠近活动边界 `16px` 内且 `LBUTTON` 按下时显示对应边的 `2px` 哨兵）。
  - 在 `FileDropBridge.cpp:427-462`（`SentinelDropTarget::DragEnter`）确保非 `CF_HDROP` 数据对象返回 `DROPEFFECT_NONE` 且**绝不调用 `armNativeDragHandoff`**，并仅在本地未进入提交取消期且确有文件时接受捕获。
  - 在 `FileDropBridge.cpp:890-908`（`onLeftMouseUp`）无论 `onSecondary` 为何均调用 `g_sentinelMgr.hideAll()` 并重置 `cancellingNativeDrag` / `nativeDragCancellationComplete` / 挂起定时器与临时未出屏文件列表；完善 `canSwitchFromPrimary()` + `cancelNativeDragBeforeSwitch()` 对已到期/取消状态的一致性清理。

- [ ] **Step 5: 验证测试通过并提交**

  Run: `cmake --build deskflow-master/build && ctest --test-dir deskflow-master/build/src/unittests --output-on-failure -R "ServerTests|FileDropBridgeTests|DragHandoffTests"`
  Expected: PASS
  ```bash
  git add deskflow-master/src/lib/server/Server.cpp deskflow-master/src/lib/platform/MSWindowsHook.cpp deskflow-master/src/lib/deskflow/FileDropBridge.cpp deskflow-master/src/unittests/
  git commit -m "fix: align sentinel thickness with jump zone, fix bottom switch wait sign and drag cancellation races"
  ```

---

### Task 3: 双向 `kMsgDDragInfo` 线协议与精确在线边段计算模块（`DragProtocol`）

**Files:**
- Create: `deskflow-master/src/lib/deskflow/DragProtocol.h`
- Create: `deskflow-master/src/lib/deskflow/DragProtocol.cpp`
- Modify: `deskflow-master/src/lib/deskflow/CMakeLists.txt`
- Modify: `deskflow-master/src/lib/server/ClientProxy1_5.h`
- Modify: `deskflow-master/src/lib/server/ClientProxy1_5.cpp:26-58`
- Modify: `deskflow-master/src/lib/client/ServerProxy.h:50-55`
- Modify: `deskflow-master/src/lib/client/ServerProxy.cpp:312-324`
- Modify: `deskflow-master/src/lib/client/Client.h:165-175`
- Modify: `deskflow-master/src/lib/client/Client.cpp`
- Test: `deskflow-master/src/unittests/deskflow/FileDropBridgeTests.cpp`
- Test: `deskflow-master/src/unittests/server/ClientProxyTests.cpp`
- Test: `deskflow-master/src/unittests/client/ServerProxyTests.cpp`

**Interfaces:**
- Produces (`deskflow::drag` namespace in `DragProtocol.h`):
  - `enum class DragEdgeSide : uint8_t { Left = 0, Right = 1, Top = 2, Bottom = 3 };`
  - `struct DragEdgeSegment { DragEdgeSide side; int32_t startPx; int32_t endPx; bool operator==(const DragEdgeSegment &) const = default; };`
  - `std::string serializeEdgeSegments(const std::vector<DragEdgeSegment> &segments);`
  - `std::vector<DragEdgeSegment> parseEdgeSegments(std::string_view payload);`
  - `std::vector<DragEdgeSegment> computeClippedEdgeSegments(DragEdgeSide side, int32_t axisOrigin, int32_t axisExtent, const std::vector<std::pair<float, float>> &connectedIntervals, int32_t cornerStartSizePx, int32_t cornerEndSizePx);`
  - `std::string formatPeersMessage(const std::vector<std::string> &ips);` / `std::vector<std::string> parsePeersMessage(std::string_view payload);`
  - `std::string redactDragLogPayload(std::string_view info);`
  - `struct ParsedServerDragCommand` / `ParsedClientDragCommand` + `parseServerDragCommand(std::string_view)` / `parseClientDragCommand(std::string_view)`（覆盖 `DESKSEND_EDGES:`、`DESKSEND_PEERS:`、`DESKSEND_COMMIT_HANDOFF:`、`DESKSEND_START:`、`DESKSEND_DROP:`、`DESKSEND_CANCEL`、`DESKSEND_SEND_DROP:`、`DESKSEND_SEND_CANCEL` 及上行 `DESKSEND_CLIENT_PRECAPTURE:`、`DESKSEND_CLIENT_PRECAPTURE_CLEAR`、`DESKSEND_HANDOFF_ACK:`）。
- Produces (`ServerProxy` / `Client` / `Server` 虚或回调入口):
  - `void ServerProxy::sendDragInfo(uint32_t fileCount, const char *info, size_t size);`
  - `void Client::sendDragInfo(uint32_t fileCount, const std::string &info);`
  - `void Server::onClientDragInfo(BaseClientProxy *client, uint16_t fileCount, const std::string &info);`（可测试入口；`ClientProxy1_5::dragInfoReceived()` 读取后在 `getServer() != nullptr` 时回调）。

- [ ] **Step 1: 为 `DragProtocol` 编解码、角锁裁剪与 `ClientProxy1_5::dragInfoReceived` 流对齐写失败单测**

  - 在 `FileDropBridgeTests.cpp` 添加 `dragProtocol_serializesParsesEdgesAndClipsCorners()`：验证边 `[0, 1080)` 邻接区间 `[0.25, 1.0]` 且底角锁 `100px` 裁剪为 `[270, 980)`；空邻接区间产生空段；`DESKSEND_PEERS:` 去重与解析；`redactDragLogPayload` 对全部含 token 指令打码。
  - 在 `ClientProxyTests.cpp` 添加 `dragInfoReceived_consumesStreamBeforeNextKeepAlive()`（对齐 **Review Focus #2**）：向假读入流先后写入 `kMsgDDragInfo(2, "DESKSEND_CLIENT_PRECAPTURE:2:1")` 与 `kMsgCKeepAlive`，通过 `ClientProxy1_6::handleData()` 驱动解析，断言 `kMsgCKeepAlive` 也成功解析而非引发未知协议错位断连。
  - 在 `ServerProxyTests.cpp` 添加 `sendDragInfo_writesDdrgFrameAndRedactsSensitiveLogs()`：验证 `ServerProxy::sendDragInfo` 写出正确 `"DDRG"` 长度头包；收到 `DESKSEND_COMMIT_HANDOFF:<secret>` 或 `DESKSEND_SEND_CANCEL:<secret>` 正确路由。

- [ ] **Step 2: 运行单测确认失败**

  Run: `ctest --test-dir deskflow-master/build/src/unittests --output-on-failure -R "FileDropBridgeTests|ClientProxyTests|ServerProxyTests"`
  Expected: FAIL（`DragProtocol.h` 不存在、`ClientProxy1_5::dragInfoReceived()` 未读流、`ServerProxy::sendDragInfo` 不存在）。

- [ ] **Step 3: 实现 `DragProtocol.h/.cpp` 并加入 `deskflow/CMakeLists.txt`**

  - 实现无 UI/无 Win32 依赖的纯 C++ `DragProtocol`（各前缀解析、区间裁剪、角锁与负坐标虚拟屏兼容、日志脱敏）。

- [ ] **Step 4: 修复 `ClientProxy1_5::dragInfoReceived` 并补全 `ServerProxy::sendDragInfo` / `Client::sendDragInfo` / `Server::onClientDragInfo` 桩与路由入口**

  - `ClientProxy1_5.h/.cpp`：去掉 `dragInfoReceived()` 的 `const` 限定，用 `ProtocolUtil::readf(getStream(), kMsgDDragInfo + 4, &fileCount, &dragInfo)` 消费流字节并脱敏打 `LOG_INFO`，若 `getServer() != nullptr` 则调用 `getServer()->onClientDragInfo(this, fileCount, dragInfo)`。
  - `ServerProxy.h/.cpp`：实现 `sendDragInfo(...)` 调用 `ProtocolUtil::writef(m_stream, kMsgDDragInfo, fileCount, &data)`；更新 `parseMessage` 中 `kMsgDDragInfo` 日志脱敏使用 `redactDragLogPayload`。
  - `Client.h/.cpp`：添加 `sendDragInfo`（调用 `m_server->sendDragInfo(...)` 并在非连接态安全早返回）。
  - `Server.h/.cpp`：声明并添加 `onClientDragInfo(BaseClientProxy *client, uint16_t fileCount, const std::string &info)`（Task 6 接入完整状态机）。

- [ ] **Step 5: 验证测试通过并提交**

  Run: `cmake --build deskflow-master/build && ctest --test-dir deskflow-master/build/src/unittests --output-on-failure -R "FileDropBridgeTests|ClientProxyTests|ServerProxyTests"`
  Expected: PASS
  ```bash
  git add deskflow-master/src/lib/deskflow/DragProtocol.h deskflow-master/src/lib/deskflow/DragProtocol.cpp deskflow-master/src/lib/deskflow/CMakeLists.txt deskflow-master/src/lib/server/ deskflow-master/src/lib/client/ deskflow-master/src/unittests/
  git commit -m "feat(protocol): add bidirectional kMsgDDragInfo edge segment codec and fix Server stream read"
  ```

---

### Task 4: 光标跟随胶囊（`DragHoverIndicator`）与 `TransferStatus` 即时反馈联动

**Files:**
- Create: `deskflow-master/src/lib/deskflow/DragOverlayWidgets.h`
- Create: `deskflow-master/src/lib/deskflow/DragOverlayWidgets.cpp`
- Modify: `deskflow-master/src/lib/deskflow/CMakeLists.txt`
- Modify: `deskflow-master/src/lib/deskflow/FileDropBridge.cpp:71-225,1651-1785`
- Test: `deskflow-master/src/unittests/deskflow/FileDropBridgeTests.cpp`

**Interfaces:**
- Produces (`deskflow` 命名空间 `DragOverlayWidgets.h`):
  - `class DragHoverIndicator final : public QWidget`:
    - `void showForFiles(size_t fileCount);`
    - `void hideIndicator();`
    - `static QString formatBannerText(size_t fileCount);`
    - `static QPoint clampIndicatorPosition(const QPoint &cursorLogical, const QSize &widgetSize, const QRect &availableGeom);`
  - `class TransferStatus final : public QWidget`:
    - `explicit TransferStatus(bool receiving, QWidget *parent = nullptr);`
    - `void beginPreparing(const QString &description);`
    - `void begin(const QString &description);`
    - `void updateProgress(uint64_t sent, uint64_t total);`
    - `void finish(bool cancelled, bool failed, const QString &message);`

- [ ] **Step 1: 为 `DragHoverIndicator` 定位防裁切、`DESKSEND_START/DROP/CANCEL` 胶囊联动与 `beginPreparing` 状态写失败单测**

  - 在 `FileDropBridgeTests.cpp` 添加：
    - `dragHoverIndicator_clampsToScreenEdgesAndFormatsCount()`：测试 `1920x1080` 右下角 `(1900, 1060)` 坐标下 `clampIndicatorPosition` 自动向左/上方翻转保证完全在 `availableGeom` 内，以及 `formatBannerText(3)` / `formatBannerText(0)` 返回正确中文文案。
    - `dragHoverAndTransferStatus_transitionsOnStartDropCancel()`：在 `DESKSEND_START:<tok>` 收到后验证接收侧“悬浮提示激活且携带文件数正确”，收到 `DESKSEND_DROP:<x>,<y>:<tag>` 后立即验证悬浮隐藏且接收端 `TransferStatus` 进入非空 preparing 状态（不等 `RECEIVE_SESSION_STARTED`）；以及 `DESKSEND_CANCEL` 立即隐藏悬浮。

- [ ] **Step 2: 运行单测确认失败**

  Run: `ctest --test-dir deskflow-master/build/src/unittests --output-on-failure -R "FileDropBridgeTests"`
  Expected: FAIL（`DragOverlayWidgets.h` 及 `beginPreparing` 不存在）。

- [ ] **Step 3: 实现 `DragOverlayWidgets.h/.cpp`（含 Windows `WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_LAYERED` 穿透）**

  - 将原 `FileDropBridge.cpp:71-176` 的 `TransferStatus` 迁入独立模块并新增 `beginPreparing(const QString &description)` 与 `~2.5s` 自动隐退 `QTimer`（重新进入 `beginPreparing` / `begin` 时立即 `stop()` 阻止旧定时器误隐藏新传输）；注意构造/展示前确保 `QApplication::instance() != nullptr`，在无 Qt GUI 实例的无头单测下安全降级跳过窗口创建而不崩溃。
  - 实现 `DragHoverIndicator`：无边框圆角半透明胶囊，绑定 16ms UI `QTimer`（仅可见时运行）采样 `QCursor::pos()` + `QGuiApplication::screenAt(pos)->availableGeometry()`，在 `#if defined(_WIN32)` 的 `showEvent` 中用 `SetWindowLongPtrW(..., GWL_EXSTYLE, ...)` 确保鼠标点击与 `DragEnter` 透明穿透。

- [ ] **Step 4: 在 `FileDropBridge.cpp` 接入 `DragHoverIndicator` 与 `beginPreparing()` 并修改 `CMakeLists.txt`**

  - `DESKSEND_START`：调用胶囊 `showForFiles(fileCount)`；
  - `DESKSEND_DROP`：同步隐藏胶囊并调用 `transferPreparing(true, ...)`；
  - `DESKSEND_CANCEL`：取消匹配 token 时隐藏胶囊。

- [ ] **Step 5: 验证测试通过并提交**

  Run: `cmake --build deskflow-master/build && ctest --test-dir deskflow-master/build/src/unittests --output-on-failure -R "FileDropBridgeTests"`
  Expected: PASS
  ```bash
  git add deskflow-master/src/lib/deskflow/DragOverlayWidgets.h deskflow-master/src/lib/deskflow/DragOverlayWidgets.cpp deskflow-master/src/lib/deskflow/FileDropBridge.cpp deskflow-master/src/lib/deskflow/CMakeLists.txt deskflow-master/src/unittests/
  git commit -m "feat(ui): add click-through cursor drag pill and immediate preparing transfer status"
  ```

---

### Task 5: `DropAgent` 权限边界拆分与 `SYSTEM` 托管子进程（`--drop-agent`）

**Files:**
- Create: `deskflow-master/src/lib/deskflow/DropAgentIpc.h`
- Create: `deskflow-master/src/lib/deskflow/DropAgentIpc.cpp`
- Create: `deskflow-master/src/lib/deskflow/DropExecutor.h`
- Create: `deskflow-master/src/lib/deskflow/DropExecutor.cpp`
- Create: `deskflow-master/src/lib/deskflow/DropAgentMain.h`
- Create: `deskflow-master/src/lib/deskflow/DropAgentMain.cpp`
- Modify: `deskflow-master/src/lib/deskflow/CMakeLists.txt`
- Modify: `deskflow-master/src/lib/deskflow/FileDropBridge.cpp`（把 `Win32SentinelManager`、`localsend-cli` 收发、`resolveDropTargetDirectory`、`copyFilesToWindowsClipboard` 与浮窗操作委派给 `IDropExecutor` / `DropAgentHost`）
- Modify: `deskflow-master/src/apps/deskflow-core/CoreArgs.h`
- Modify: `deskflow-master/src/apps/deskflow-core/CoreArgParser.h`
- Modify: `deskflow-master/src/apps/deskflow-core/CoreArgParser.cpp`
- Modify: `deskflow-master/src/apps/deskflow-core/deskflow-core.cpp:85-120`
- Test: `deskflow-master/src/unittests/deskflow/FileDropBridgeTests.cpp`

**Interfaces:**
- Produces (`deskflow::drop` / `deskflow`):
  - `DropAgentIpc.h`: JSON-Line 编解码 `encodeAgentCommand(...)` / `decodeAgentCommand(...)`、`encodeAgentEvent(...)` / `decodeAgentEvent(...)`（含 `ConfigureReceiver`、`GrantToken`、`RevokeToken`、`SetSentinels`、`HideSentinels`、`DropCommit`、`SendFiles`、`ShowHover`、`HideHover` ↔ `PreCapture`、`DragLeave`、`TransferActive`、`ReceiveStateChanged`、`SendCompleted`）。
  - `DropExecutor.h`:
    - `class IDropExecutor`（接收控制、token `GRANT`/`REVOKE`、哨兵区间刷新与隐藏、落点投递、发件执行、悬浮/准备状态提示与回调注册）
    - `class InProcessDropExecutor final : public IDropExecutor`（直接持有并运行 `DropAgentHost`，同时承担无提权运行及单元测试的接收队列管理；含 `hasActiveTransferOrDrag()` 检查从而实现**延迟重启接收器**！）
    - `class OutOfProcessDropExecutor final : public IDropExecutor`（Windows 检测到当前进程为 `SYSTEM` 或高完整性提权时启用，通过 `WTSGetActiveConsoleSessionId` + `MSWindowsSession::getUserToken(false)` / `GetShellWindow()` 回退 + `CreateEnvironmentBlock` + `KillOnCloseJob` + 继承匿名 `CreatePipe` 启动 `deskflow-core.exe --drop-agent`；锁屏/无登录用户时缓存最新期望配置并安全延后拉起，待用户登录后立即回放配置）
  - `DropAgentMain.h`: `int runDropAgentMain();`
  - `CoreArgParser::dropAgentMode() const;`（对应 `--drop-agent`）

- [ ] **Step 1: 为 `DropAgentIpc` 编解码、延迟接收器白名单重启、`--drop-agent` 参数解析及无实例锁早入口写失败单测**

  - 在 `FileDropBridgeTests.cpp` 添加：
    - `dropAgentIpc_roundTripsAllCommandsAndEvents()`：覆盖含空格/中文/特殊路径文件数组、`DragEdgeSegment` 列表与 token 的 JSON-Line 往返一致性。
    - `dropExecutor_defersReceiverRestartWhileTransferOrDragActive()`（对齐 **Review Focus #3**）：启动模拟接收并标记活跃接收或挂起 `DESKSEND_START` 后，先后以新允许列表 `{"192.168.1.20", "192.168.1.21"}` 请求配置更新，断言在传输/拖拽期间当前接收代数 `m_receiverGeneration` 与口令队列**保持不变**，且当传递会话结束事件变为 idle 后立刻自动应用挂起允许列表重启。
    - 校验 `CoreArgParser({"deskflow-core", "--drop-agent"})` 解析得到 `dropAgentMode() == true` 且不输出错误帮助信息（对齐 **Review Focus #5**）。

- [ ] **Step 2: 运行单测确认失败**

  Run: `ctest --test-dir deskflow-master/build/src/unittests --output-on-failure -R "FileDropBridgeTests"`
  Expected: FAIL（`DropAgentIpc.h`、`DropExecutor.h` 及 `dropAgentMode()` 不存在）。

- [ ] **Step 3: 实现 `DropAgentIpc.h/.cpp` 与 `IDropExecutor` / `DropAgentHost`（含延迟白名单重建逻辑）**

  - 抽离 `DropAgentHost`（承载所有用户态职责：`Win32SentinelManager` 按 `DragEdgeSegment` 精细区间定位各边缘哨兵、`localsend-cli` 接收与发射队列、`resolveDropTargetDirectory`、`copyFilesToWindowsClipboard`、`DragHoverIndicator`、`TransferStatus`）。
  - 实现 `InProcessDropExecutor` 与延后重启逻辑（保存 `m_pendingAllowSpec` 标记，待传输与活动拖拽归零时自动刷新）。

- [ ] **Step 4: 实现 Windows `OutOfProcessDropExecutor`（匿名管道 + 用户令牌 + `KillOnCloseJob` + 会话变更自动重拉）与 `DropAgentMain`**

  - 通过 `ArchMiscWindows::isProcessElevated()` 或检查自身是否 `SYSTEM` 令牌选择 `OutOfProcessDropExecutor`（也可通过构造函数/测试注入自定义 `IDropExecutor`）；
  - 启动子进程：仅继承读写管道端句柄（父端设为 `HANDLE_FLAG_INHERIT = 0`），通过 `CreateProcessAsUserW` + `winsta0\Default` + `CreateEnvironmentBlock` 运行 `"<selfExe>" --drop-agent`；锁屏时捕获无活动会话失败，记录日志不崩主核心并在有事件/定时器检查用户登录后拉起并重放缓存的 `ConfigureReceiver` / `SetSentinels`。
  - `DropAgentMain` 在子进程主线程创建 `QApplication`，工作管道读线程解析父进程指令并在 UI/Win32 线程调度执行 `DropAgentHost`（同线程拥有带消息泵的隐藏 `HWND` 或使用工作消息泵驱动 `Win32SentinelManager` `OleInitialize` + `RegisterDragDrop`！注：`OleInitialize` 所在 STA 线程必须持续运行 Win32/Qt 消息泵，放在 `--drop-agent` 的 Qt 主线程直接调用 `Win32SentinelManager` 即共享 `QApplication` 的 Windows UI 消息泵，天然支持 STA COM + `RegisterDragDrop` + Qt 浮窗！）。

- [ ] **Step 5: 在 `CoreArgs.h` / `CoreArgParser` / `deskflow-core.cpp` 接入 `--drop-agent` 早派发**

  - 在 `deskflow-core.cpp` 的 `CoreArgParser parser(...)` 解析后、`QSharedMemory sharedMemory(kCoreBinName)` **之前**判断 `if (parser.dropAgentMode()) return deskflow::runDropAgentMain();`，确保 `--drop-agent` 不触碰核心单实例共享内存也不创建 `CoreIpcServer`。
  - 验证在 `FileDropBridgeTests` 中对 `CoreArgParser` 及模拟 `OutOfProcessDropExecutor` 通道的测试全通过（注意 `CoreArgParser.cpp` 如需测试可直接 `#include` 或新增轻量测试）。

- [ ] **Step 6: 运行测试确认通过并提交**

  Run: `cmake --build deskflow-master/build && ctest --test-dir deskflow-master/build/src/unittests --output-on-failure -R "FileDropBridgeTests"`
  Expected: PASS
  ```bash
  git add deskflow-master/src/lib/deskflow/ deskflow-master/src/apps/deskflow-core/ deskflow-master/src/unittests/
  git commit -m "feat(drop-agent): split OLE sentinel, LocalSend and UI overlays to non-elevated user session child"
  ```

---

### Task 6: 对称双向 & 三机串联多跳状态机集成（`FileDropBridge` / `Server` / `Client`）

**Files:**
- Modify: `deskflow-master/src/lib/deskflow/FileDropBridge.h`
- Modify: `deskflow-master/src/lib/deskflow/FileDropBridge.cpp`
- Modify: `deskflow-master/src/lib/server/Server.h`
- Modify: `deskflow-master/src/lib/server/Server.cpp:230-245,380-545,1460-1475,1570-1585,1630-1695,1830-1927,1950-2000,2110-2120`
- Modify: `deskflow-master/src/lib/client/Client.h`
- Modify: `deskflow-master/src/lib/client/Client.cpp:65-90,175-215,245-355,400-500`
- Test: `deskflow-master/src/unittests/deskflow/FileDropBridgeTests.cpp`
- Test: `deskflow-master/src/unittests/server/ServerTests.cpp`

**Interfaces:**
- Produces on `deskflow::FileDropBridge`:
  - **Coordinator 统一角色方法**:
    - `void setDragMessageSender(std::function<void(uint32_t, const std::string &)> sender);`（由 `Client` 注册向上行发包）
    - `void startServerReceiver(const std::vector<std::string> &peerIps);` / `void stopServerReceiver();`
    - `void onClientMouseMove(int32_t x, int32_t y, int32_t ax, int32_t ay, int32_t aw, int32_t ah);`
    - `void onClientConnectedDragReady();` / `void onClientLeftMouseDownOrUp(bool down);`
    - `void onClientDragCaptured(std::vector<std::string> files, uint32_t edgeMask);` / `void onClientDragLeftSentinel();`
    - `void onServerDragInfoFromClient(BaseClientProxy *client, uint16_t fileCount, const std::string &info);`
    - `bool canSwitchOutOfComputer(BaseClientProxy *src, BaseClientProxy *dst);`（统一取代或扩展 `canSwitchFromPrimary()`：主机侧等待主端 `VK_ESCAPE` 完成，副机侧在 `PreCaptured` -> 发 `DESKSEND_COMMIT_HANDOFF` 进入 `HandoffPending` 等 ACK，收到 `DESKSEND_HANDOFF_ACK` 返回 `true`）
    - `bool canUnlockEdgeSwitchForDrag(const BaseClientProxy *activeComputer) const;`（供 `Server::isLockedToComputer()` 调用）
    - `void onServerSwitchComputer(BaseClientProxy *oldComputer, BaseClientProxy *newComputer, BaseClientProxy *primaryClient, int32_t dstX, int32_t dstY);`
    - `bool onServerMouseUp(BaseClientProxy *activeComputer, BaseClientProxy *primaryClient, int32_t x, int32_t y);`
    - `void onServerEscapePressed();` / `void onServerShutdown();`

- [ ] **Step 1: 为全路径双向及多跳状态机写覆盖全量 Review Focus 用例的失败单测**

  - 在 `FileDropBridgeTests.cpp` 添加 6 个关键端到端测试（使用 `MockDragClientProxy` 代表 `primaryClient`、`clientA`、`clientB` 及注入的内存/伪执行器记录指令）：
    1. `clientToPrimary_twoPhaseHandoffAndDropOnPrimary()`：模拟 `Client A` 上 `onClientDragCaptured({"C:/a.txt"}, LeftMask)` 发出 `DESKSEND_CLIENT_PRECAPTURE` 但**不发 `VK_ESCAPE`**；在 `Server` 拒绝切换（如停在角落）期间确认无 `COMMIT_HANDOFF` 发出；切换满足后发出 `DESKSEND_COMMIT_HANDOFF:<token>`，`Client` 回发 `DESKSEND_HANDOFF_ACK`，`Server` 切到 `primaryClient` 触发主屏悬浮胶囊；物理左键松开时主机授权 `GRANT <token>` + `TransferStatus::beginPreparing`，并向 `Client A` 下发 `DESKSEND_SEND_DROP:<x>,<y>:<tag>:SERVER`。
    2. `clientAToClientB_viaPrimaryMultiHop_preservesSourceFilesAndCleansPrimaryOverlay()`（对齐 **Review Focus #4**）：接上一步进入 `primaryClient` 后继续不松手切入 `Client B`；断言主机浮窗隐藏、向 `Client B` 下发 `DESKSEND_START:<token>`、**`Client A` 未收到取消**；在 `Client B` 松手时下发 `DESKSEND_DROP:...` 给 `Client B`，并向 `Client A` 下发 `DESKSEND_SEND_DROP:...:<clientBIp>`。
    3. `clientToPrimaryThenReturnToOrigin_cancelsCleanlyWithoutDrop()`（对齐 **Review Focus #4**）：从 `Client A` 拖入 `primaryClient` 后不松手退回 `Client A`，断言向 `primary` 隐藏胶囊并向 `Client A` 下发 `DESKSEND_SEND_CANCEL`，状态恢复 `Idle` 且后续普通 `onServerMouseUp` 不触发传输。
    4. `clientDisconnectDuringMultiHop_resetsLockAndNotifiesSurvivingEndpoint()`：`Client A -> Primary -> Client B` 途中 `Client A` 掉线，断言 `DESKSEND_CANCEL` 发往 `Client B`；若 `Client B` 掉线，断言向 `Client A` 发 `DESKSEND_SEND_CANCEL`；所有挂起状态清空且 `canUnlockEdgeSwitchForDrag` 归 `false`。
    5. `primaryToClient_preservedBackwardCompatibility()`：保留 `主机 -> 副机` 正向拖拽从哨兵捕获到 `DESKSEND_START` / `DESKSEND_DROP` / `DESKSEND_CANCEL` 全流程兼容。
  - 在 `ServerTests.cpp` 添加服务端拓扑构造单测 `computeClientDragSegments_excludesDisconnectedNeighborsAndCorners()`。

- [ ] **Step 2: 运行测试确认失败**

  Run: `ctest --test-dir deskflow-master/build/src/unittests --output-on-failure -R "FileDropBridgeTests|ServerTests"`
  Expected: FAIL（上述协调方法与状态机尚未合入）。

- [ ] **Step 3: 实现 `FileDropBridge` 统一 `ActiveCrossDrag` 状态机与 Client/Server 双侧驱动**

  - 在 `FileDropBridge.h/.cpp`：
    - 统一维护 `ActiveCrossDrag`（`Idle` / `PreCaptured` / `HandoffPending` / `ActiveCrossScreen`）与 150ms 超时保护避免 `HandoffPending` 孤立卡死；
    - 实现客户端侧 `onClientMouseMove`（依据 `DESKSEND_EDGES` 下发的精确 `DragEdgeSegment` 判断当前指针靠近哪个有效边缘段时显示哨兵）、二段式 `onClientDragCaptured`（暂存本地文件并发送 `DESKSEND_CLIENT_PRECAPTURE`，等待 `DESKSEND_COMMIT_HANDOFF` 后再在核心进程发 `VK_ESCAPE` + 本地 `LEFTUP`、隐藏哨兵并上报 `DESKSEND_HANDOFF_ACK`）、以及收到 `DESKSEND_SEND_DROP`（向 `serverIp` 或 `<targetIp>` 发 `SendFiles`）与 `DESKSEND_SEND_CANCEL`；
    - 实现服务端侧 `onServerSwitchComputer` / `onServerMouseUp` / `onServerEscapePressed` / `onClientDisconnected` 多跳接力、回源取消与直接 `Primary` 本地投递；
    - 保证主机接收或副机接收完成后都把 `Client A`'s IP（或 `SERVER` IP）正确传递至发送队列。

- [ ] **Step 4: 将 `Server` 与 `Client` 事件流接入 `FileDropBridge`**

  - `Server.cpp`：
    - 新增 `Server::updateDragTopologyAndPeers()` 辅助方法：遍历 `m_clients`（跳过 `m_primaryClient`），读取 `Config` 邻居迭代器和目标 `getCornerSize` 算出各客户端精确 `DragEdgeSegment` 列表下发 `DESKSEND_EDGES:...`；汇总全量客户端 `getClientPeerIp` 更新主机接收器 `startServerReceiver(...)` 及各客户端 `DESKSEND_PEERS:...`（在 `start`、`setConfig`、`addClient`、`removeClient`、`ComputerShapeChanged` 调用，同时更新 `Server::getActivePrimarySides()`）。
    - `isLockedToComputer()`（`L383-391`）：改调用 `FileDropBridge::instance().canUnlockEdgeSwitchForDrag(m_active)` 统一放行主端/副端有效拖拽（保留常规按住左键时的屏内锁）。
    - `switchComputer()`（`L409-525`）：调用 `canSwitchOutOfComputer(m_active, dst)`（若返回 `false` 进入短等待如 `startSwitchWait(x, y, 0.12)`，收到 `DESKSEND_HANDOFF_ACK` 后若 `m_switchComputer != nullptr` 立刻触发切换无需等完 120ms！），并在切换完成后以统一 `onServerSwitchComputer(oldComputer, m_active, m_primaryClient, x, y)` 取代单独的 `onLeavePrimary`/`onReturnToPrimary`。
    - `onMouseMoveSecondary()`：收到 `DESKSEND_CLIENT_PRECAPTURE` 时，若游标正好停在激活边缘且有向邻接边缘移动意图，重触发一次边界检测以便及时进入 `HandoffPending`。
    - `onMouseUp()` / `onKeyDown(kKeyEscape)` / `closeServer()` / `removeClient()`：接入统一回调并传入目标 peer IP / 清理状态。
  - `Client.cpp`：
    - `setupComputer` / `handshakeComplete()` / `cleanupServer()` 注册并清理 `FileDropBridge::setDragMessageSender(...)`；
    - `mouseMove(xAbs, yAbs)` 调用 `onClientMouseMove(xAbs, yAbs, ax, ay, aw, ah)`；`mouseDown(kButtonLeft)` / `mouseUp(kButtonLeft)` 更新客户端按键状态；`leave()` 隐藏副机本地哨兵及收起胶囊。

- [ ] **Step 5: 验证本地测试通过（或工具链检查）并提交**

  Run: `cmake --build deskflow-master/build && ctest --test-dir deskflow-master/build/src/unittests --output-on-failure -R "FileDropBridgeTests|ServerTests|DragHandoffTests|ServerProxyTests|ClientProxyTests|MSWindowsDesksTests"`
  Expected: PASS
  ```bash
  git add deskflow-master/src/lib/deskflow/ deskflow-master/src/lib/server/ deskflow-master/src/lib/client/ deskflow-master/src/unittests/
  git commit -m "feat: support symmetric bidirectional and multi-hop cross-screen file drag via shared mouse"
  ```

---

### Task 7: 跨平台编译防护核查与 GitHub Actions 全量构建/测试验收

**Files:**
- Verify: `deskflow-master/src/lib/deskflow/CMakeLists.txt`
- Verify: `.github/workflows/deskflow-win.yml`
- Check all `#if defined(_WIN32)` / `#ifdef Q_OS_WIN` guards across newly added/modified files

- [ ] **Step 1: 全量检查非 Windows 宏隔离、日志脱敏与未用引用**

  Run: `git diff --check` 与检查所有 `FileDropBridge` / `DropExecutor` / `DragOverlayWidgets` 新增头文件在非 Win32 下均无直接 Win32 API 泄露，敏感命令均已走 `redactDragLogPayload`。

- [ ] **Step 2: 推送分支至 GitHub 并实时监控 `.github/workflows/deskflow-win.yml` 构建与 CTest**

  Run: `git push origin main`（或当前开发分支，取决于用户发布偏好；注意先推送工作流运行确认全量构建+CTest 变绿，再视用户要求决定打 tag 发布 Release）。
  Monitor: `gh run watch`
  Expected: `Configure`、`Build`（含 `run_tests` 全量 `CTest` 执行）、`Package`（`deskflow-win-x64` ZIP）全部成功无报错。

---

## Self-Review Check

- **Spec coverage**：设计文档 5.1 权限拆分（`DropExecutor` + `--drop-agent`）、5.2 双端接收/延迟重启、5.3 双向 `kMsgDDragInfo` + 流修复、5.4 二段确认 + 多跳状态机、5.5 光标胶囊 + `beginPreparing`、5.6 全部卡顿/拖拽修复均完整对应至 Task 1–6，无遗漏段落。
- **Step scan**：每步测试名称与断言目标明确，生产方法给出确切文件名、位置、签名及接口契约，不包含无边界占位语句或冗长逐行实现抄录。
- **Type consistency**：统一使用 `DragEdgeSegment`、`DragEdgeSide`、`DragHoverIndicator`、`TransferStatus::beginPreparing`、`IDropExecutor`、`InProcessDropExecutor`、`OutOfProcessDropExecutor`、`runDropAgentMain()`、`CoreArgParser::dropAgentMode()`、`onClientDragInfo`、`canSwitchOutOfComputer`、`canUnlockEdgeSwitchForDrag` 等命名跨 Task 完全一致。
- **Review Focus coverage**：Top 5 故障模式分别有 Task 2 Step 1、Task 3 Step 1、Task 5 Step 1、Task 6 Step 1 中的具体测试条款直接绑定覆盖。
- **Proportion**：任务拆分简洁适中（7 个独立可验证任务），保持方案清晰可读。
