# 双向文件拖拽传输与接收端视觉反馈设计文档

- **日期**：2026-11-15
- **分支 / 基线**：`main`（`eb4f7bc` / `v1.0.5`）
- **状态**：已根据审阅反馈与用户决策定稿（待用户最终批准实施）

---

## 1. 背景与目标（Intent & Why）

当前 DeskSend 已实现从**主机（Server / Primary）向副机（Client / Secondary）**跨屏拖放文件（基于 `kMsgDDragInfo` 控制信令 + `localsend-cli` 局域网 HTTPS 直传），但在默认安装模式与日常使用中存在三类核心问题：

1. **接收端拖放与传输缺乏即时视觉反馈**：当文件从主机拖入副机时，副机收到 `DESKSEND_START:<token>` 后仅向 `localsend-cli` 写入 `GRANT <token>` 并打日志，鼠标经过副机期间只显示普通指针；右下角 `TransferStatus` 浮窗要等到松手触发 `DESKSEND_DROP` 且 `localsend-cli` 完成 TLS/HTTP 建连发出 `RECEIVE_SESSION_STARTED` 后才弹出，导致用户在光标和目标窗口旁完全看不出“正携带文件可松手”。
2. **文件传输仅支持单向（主机 → 副机）**：副机未创建跨屏边缘哨兵，主机从未启动 `localsend-cli` 接收器，服务端对副机上行 `kMsgDDragInfo` 是未读流的空桩（`ClientProxy1_5::dragInfoReceived`），且用户在副机按住左键拖拽时会触发 `Server::isLockedToComputer()` 锁死在当前屏幕，无法把副机文件拖回主机或跨越多台副机拖放。
3. **默认提权服务模式下的 UIPI 边界与已排查基础稳定性问题**：
   - Windows 安装版默认以 `Core::ProcessMode = Service` 且 `Daemon::Elevate = true` 运行，`deskflow-core.exe` 持有 `NT AUTHORITY\SYSTEM` + `UIAccess` 令牌；当前直接在 `deskflow-core` 内运行的 OLE `RegisterDragDrop`、`FOLDERID_Desktop` / `CLSID_ShellWindows`、`QStandardPaths::DownloadLocation` 受 Windows UIPI 跨权限拦截或落入系统目录，必须在保持“能控 UAC 弹窗”的同时解决拖放的用户会话边界问题。
   - 上一轮排查出的鼠标卡顿与拖拽基础缺陷（`warpCursorNoFlush` 空转、`PacketStreamFilter::write` 分包、`ServerProxy` + `ClientProxy1_3` 高频保活消息与定时器重建、`deskMouseRelativeMove` 重复 `SystemParametersInfo` 与同步等待、`8px` 哨兵 vs `1px` 切屏区冲突、`VK_ESCAPE` 竞态、非文件拖拽拦截与 `Server.cpp` `+ zoneSize` 符号错误等）一并合入本轮解决。

---

## 2. 范围、非目标与反目标（Goals, Non-Goals, Anti-Goals）

### Goals
- **双向与多跳跨屏文件拖拽（沿主机共享鼠标）**：用户继续使用主机共享键鼠，可从 `主机 → 副机`、`副机 → 主机`，以及在多机配置拓扑下从 `副机 A → 主机 → 副机 B`（全程不松手多跳穿越）把桌面 / 资源管理器选中的文件拖放至最终目标屏的文件夹、桌面或剪贴板。
- **原生兼容 Windows 安装版 `Service + Elevate = true` 与桌面/便携模式**：在开启 UAC 提权时，UIPI 跨权限与系统配置路径不阻碍 OLE 捕获、目录解析与发送/接收。
- **接收端两段式即时视觉反馈（主机、副机通用）**：
  - **拖入悬浮阶段（松手前）**：在目标屏幕光标旁显示轻量、不抢焦点且鼠标事件穿透的跟随胶囊，显示携带文件数与松手提示；
  - **松手接收阶段**：松手瞬间立即进入“正在准备接收…”，衔接实时进度、速率与完成后结果摘要。
- **打包修复已排查的基础卡顿与拖拽可靠性缺陷**。

### Non-Goals
- **不支持副机本地物理鼠标直拖触发切屏**：用户明确继续采用主机共享鼠标，副机自身物理鼠标不抢占主机控制权。
- **不通过 `kMsgDFileTransfer` 在 Deskflow 主控链路传文件**：文件字节流继续全部由 `localsend-cli` 走独立局域网 HTTPS 通道。
- **不针对 macOS/Linux 扩展平台桌面级哨兵**（保持现有跨平台代码和条件编译正常构建）。

### Anti-Goals
- **不得拦截非文件拖拽或窗口拖动**：在边缘拖动窗口、划选文本、拖拽非 `CF_HDROP` 内容时，不得误取消操作或阻挡屏幕内操作。
- **不得在非接管点或未满足切屏条件时误取消 Explorer 拖拽**：在锁定角落（`switchCornerSize`）、未连通边段、未上线邻居或切屏延迟（`switchDelay`）未满足时，不得提前发送 `VK_ESCAPE` 毁掉用户正在进行的本地拖拽。
- **不得对下层普通用户暴露高危跨权限本地提权面**：`SYSTEM` 核心与非提权用户态代理间不得使用无认证的全局命名管道执行任意路径/命令。

---

## 3. 约束与环境前提（Constraints）

- **进程与令牌模型（Windows）**：
  - 安装模式下，`Settings.cpp` 默认 `Core::ProcessMode = Service` 且 `Daemon::Elevate = true`，`deskflow-daemon` 通过 `MSWindowsWatchdog::getUserToken(true)` 复制 `winlogon.exe` 的 **`NT AUTHORITY\SYSTEM` + `TokenUIAccess=1`** 令牌启动 `deskflow-core.exe`（位于当前活跃控制台会话桌面）。
  - 桌面/便携/关闭提权模式下，`deskflow-core.exe` 直接以当前交互用户的 Medium-IL 令牌运行。
  - 当前 `deskflow-core.cpp` 主线程持有 `QApplication`，工作线程 `coreThread` 运行 `EventQueue` 与 Win32 消息循环，且非特殊参数路径会创建 `QSharedMemory(kCoreBinName)` 单实例锁和 `CoreIpcServer`。
- **LocalSend 端口与配置隔离**：`localsend-cli` 接收器监听固定端口 `53318`（可通过 `DESKSEND_RECEIVER_PORT` 覆盖）并启用 `--auth-stdin` + `--auto-accept-from`；发送器每次以 `--port 0` 动态分配端口且与接收器使用分离的 `XDG_CONFIG_HOME`（`sender` / `receiver`），同机双向收发无端口与配置冲突。
- **协议向后兼容**：保留 `kMsgDDragInfo`（`"DDRG%2i%s"`）既有线格式，不修改协议主次版本号；保持原有 `DESKSEND_START:<token>`、`DESKSEND_DROP:<x>,<y>[:<tag>]`、`DESKSEND_CANCEL[:<token>]` 解析兼容，确保既有测试用例继续有效。
- **构建与验证**：Windows Release 构建与 `CTest` 在 GitHub Actions（`.github/workflows/deskflow-win.yml`）完成。

---

## 4. 已评估方案与选型（Approaches）

### 4.1 双向文件传输与权限边界方案
1. **已选定方案：对称协议状态机 + `deskflow-core` 双执行域（非提权同进程直调 / `SYSTEM` 下按用户令牌托管 `--drop-agent`）**
   - **做法**：输入钩子、`SendInput` 合成（含跨 IL 取消拖放的 `VK_ESCAPE`/`LEFTUP`）、`Server/Client` 状态机与 `kMsgDDragInfo` 协议留在 `deskflow-core`；涉及 Medium-IL UIPI 与用户目录的操作（`DeskflowSentinel` `RegisterDragDrop`、桌面/Explorer 目录识别、`DragHoverIndicator`、`TransferStatus`、`localsend-cli` 子进程与剪贴板）封装为统一执行器接口：非提权运行直接同进程执行；检测到处于 `SYSTEM`/高完整性运行时，通过 `WTSQueryUserToken`（高权限桌面场景回退壳进程令牌）以当前登录用户非提权令牌启动 `deskflow-core.exe --drop-agent`，经继承匿名管道通信。
   - **为何不直接复用 `deskflow-gui` 作为唯一代理**：Windows 服务开机或无界面后台运行时用户可能关闭 `deskflow-gui`；同二进制 `--drop-agent` 由核心按需托管并绑定 `KillOnCloseJob`，无新增安装包可执行文件且无论 GUI 是否打开均可用。
2. **已舍弃方案**：仅依赖用户关闭 `Daemon::Elevate`（方案 Y，无法满足默认安装版同时控制 UAC 与双向拖拽的要求）；快捷键触发回传（非拖拽交互）；`kMsgDFileTransfer` 走主控通道（造成卡顿）。

### 4.2 接收端视觉反馈方案
1. **已选定方案：光标跟随轻量胶囊（松手前） + 右下角 `TransferStatus` 即时衔接（松手后）**
   - 相比仅保留右下角通知框，在鼠标旁提供低干扰、鼠标事件全透明的即时提示，解决大屏拖拽视线焦点丢失问题；`DESKSEND_DROP` 到达瞬间立刻展示准备状态，消除 TLS 握手等待期间的视觉空白。

---

## 5. 详细设计（Architecture & Mechanics）

### 5.1 权限拆分架构：`Coordinator`（`deskflow-core`）与 `DropAgent`（用户会话执行器）

1. **职责划分**：
   - **`FileDropBridge` Coordinator（位于 `deskflow-core` 进程内）**：
     - 管理全网拖拽状态机、`kMsgDDragInfo` 收发解析、token 生成/哈希标签（`computeTokenTag`）与状态日志（`activeTransfers=1/0` 供 GUI 状态感知）。
     - 负责 `Server::isLockedToComputer()` 与切屏门控协调。
     - **保留高权限输入注入**：取消原屏 OLE 拖拽所需的 `VK_ESCAPE`（及副机防卡键所需的本地 `LEFTUP`）由 `deskflow-core`（拥有 `UIAccess` 时可跨越任意完整性级别）执行，不交由低权限子进程，确保即使拖拽源是管理员权限软件也能可靠终止 `DoDragDrop`。
   - **`DropAgent` Executor（始终运行于当前登录用户的非提权桌面上下文）**：
     - 托管 `Win32SentinelManager`（各启用边缘段的 `DeskflowSentinel` 窗口、`OleInitialize`、`RegisterDragDrop`、`CF_HDROP` 提取）。
     - 执行 `resolveDropTargetDirectory(x, y)`（`WindowFromPoint`、`FOLDERID_Desktop`、`CLSID_ShellWindows`）和 `copyFilesToWindowsClipboard`。
     - 管理常驻 `localsend-cli` 接收进程与串行 `localsend-cli send` 发送队列，落盘到真实用户的下载/桌面/目标目录。
     - 渲染 `DragHoverIndicator`（光标跟随胶囊）与 `TransferStatus`（右下角进度卡片）。

2. **进程托管与安全匿名管道（Windows `SYSTEM` / 提权模式）**：
   - 启动条件检查：启动 `FileDropBridge` 平台能力时判断当前进程完整性/令牌（若非 `SYSTEM` 且非提权，直接同进程运行 `DropAgent`；若为 `SYSTEM` 或高完整性提权，则启用子进程托管模式）。
   - 子进程启动（`deskflow-core.exe --drop-agent`）：
     - 在 `deskflow-core.cpp` 中解析到 `--drop-agent` 后，**立即在单实例 `QSharedMemory(kCoreBinName)` 检查与 `CoreIpcServer` 监听之前**进入 `runDropAgentMain()`（仅初始化 `QApplication(QuitOnLastWindowClosed=false)` 与 `--drop-agent` 消息循环，不抢占主核心实例锁）。
     - 若核心为 `SYSTEM`：通过 `WTSGetActiveConsoleSessionId()` + `MSWindowsSession::getUserToken(false)` 获取活跃登录用户的**非提权主令牌**；若核心为非 `SYSTEM` 的提权管理员桌面进程：通过 `GetShellWindow()` 所属进程复制当前桌面非提权用户令牌；搭配 `CreateEnvironmentBlock` 注入真实用户环境变量并指定 `winsta0\Default`、`CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT`。
     - 子进程句柄加入 `KillOnCloseJob`，确保核心退出或崩溃时 `--drop-agent` 同步清理；若核心发现用户切换会话 / `--drop-agent` 意外退出（或启动时暂无人登录），在下次有效桌面用户就绪或有配置刷新时自动拉起并重放最新接收器允许列表与边缘配置。
   - **安全通信**：
     - 父进程通过 `CreatePipe` 创建一对**仅对子进程句柄继承**的匿名管道（`stdin` 下发控制指令，`stdout` 上报事件与进度/传输状态变化），通过 `STARTUPINFOW` 传递；**不在系统全局命名空间暴露任何可读写的公共管道名**，从而阻断其它本机低权限用户进程伪造拖拽或路径指令。
     - 协议为单行 UTF-8 JSON-Line 消息（如 `CONFIGURE_RECEIVER`、`SET_SENTINELS`、`DROP_COMMIT`、`SEND_FILES`、`SHOW_HOVER`、`HIDE_HOVER` ↔ `PRECAPTURE`、`DRAG_LEAVE`、`TRANSFER_ACTIVE`、`SEND_RESULT`、`RECEIVE_STARTED/DONE`）。

### 5.2 双端 `localsend-cli` 接收与多机信任同步

1. **客户端（`Client`）与服务端（`Server`）对称起停接收器**：
   - `Client`：握手完成后以 `serverIp`（加上随后收到的 `DESKSEND_PEERS`）启动接收器；断线时停止接收器。
   - `Server`：只要至少有 1 个客户端在线，将当前所有已连接客户端的 `FileDropBridge::getClientPeerIp(client)` 去重拼接成逗号分隔允许列表（如 `192.168.1.20,192.168.1.21`，`localsend-main` 的 `is_ip_allowed` 已原生支持逗号分隔匹配与 `--auth-stdin` token 门控），启动主机侧接收器；无客户端或服务停止时关闭。
2. **禁止拖拽中途重启接收器**：
   - `DESKSEND_PEERS` 仅在客户端连接/断开引起已连接 IP 集合变化时由 `Server` 下发；若当前正在执行接收会话或拖拽已进入目标屏，延迟到当前会话空闲后再应用接收器重启，绝不打断正在进行中的传输或丢弃已授权 token。

### 5.3 `kMsgDDragInfo` 双向信令与流修复

1. **修复 `ClientProxy1_5::dragInfoReceived`**：
   - 补全读取实现：`ProtocolUtil::readf(getStream(), kMsgDDragInfo + 4, &fileCount, &dragInfo)`，避免客户端上报时残留未读字节造成协议流错位，并路由至 `Server::onClientDragInfo(this, fileCount, dragInfo)`。
   - 新增 `ServerProxy::sendDragInfo(uint32_t fileCount, const char *info, size_t size)`，通过 `m_stream` 发送 `kMsgDDragInfo` 给 `Server`；`PrimaryClient::sendDragInfo` 保持本地无网络发送语义。
2. **完整信令集（均走 `kMsgDDragInfo`，对现有 `DESKSEND_START`/`DROP`/`CANCEL` 向后兼容）**：
   - **Server → Client**：
     - `DESKSEND_EDGES:<segments>`：下发当前客户端每条**在线且配置相邻**边缘的可切换坐标段，包含方向（`L/R/T/B`）及排除 `switchCornerSize` 后的起止坐标区间（离线配置或被角锁覆盖的边缘不上哨兵）。拓扑或客户端在线状态变化时下发。
     - `DESKSEND_PEERS:<ip1>,<ip2>,...`：通知客户端除 `serverIp` 外额外允许的兄弟副机 IP 列表。
     - `DESKSEND_COMMIT_HANDOFF:<token>`：服务端在 `mapToNeighbor` + `isSwitchOkay` + `switchDelay` 全部通过后下发给源副机，授权源副机提交切换（执行 `VK_ESCAPE` 取消本地拖拽并释放左键态，见 5.4）。
     - `DESKSEND_START:<token>`：发给当前目标副机，授权接收 token 并显示光标跟随胶囊。
     - `DESKSEND_DROP:<x>,<y>:<tag>`：发给当前目标副机，通知已松手投放并触发目录解析 + 立即进入准备接收态。
     - `DESKSEND_CANCEL[:<token>]`：发给目标副机，取消当前悬浮提示并撤销授权 token（支持多跳离开中间副机时仅清理旧目标而不销毁源端）。
     - `DESKSEND_SEND_DROP:<x>,<y>:<tag>:<target>`：发给源副机，下令把暂存文件按已授权 token 传向目标（`<target>` 为 `SERVER` 时连向客户端已保存的 `serverIp`，否则为目标副机 IP）。
     - `DESKSEND_SEND_CANCEL:<tag>`：发给源副机，通知拖拽已取消（退回源屏 / `Esc` / 目标掉线），丢弃本地挂起待发列表。
   - **Client → Server**：
     - `DESKSEND_CLIENT_PRECAPTURE:<fileCount>:<edgeMask>`：客户端 `DeskflowSentinel` 在 `DragEnter` 捕获到有效 `CF_HDROP` 时上报（不带/未提交 `VK_ESCAPE`），告知 `Server` 可解锁 `isLockedToComputer()` 参与切屏评估。
     - `DESKSEND_CLIENT_PRECAPTURE_CLEAR`：光标滑出哨兵（`DragLeave`）或客户端发现左键已提前松开时上报，撤销挂起预捕获态。
     - `DESKSEND_HANDOFF_ACK:<token>`：源副机在核心完成 `VK_ESCAPE` 取消 Explorer `DoDragDrop` 并清理副机左键状态后回复，触发 `Server` 切换屏幕；内含已生成待用 token（脱敏日志输出）。

> **日志脱敏**：`ServerProxy`、`ClientProxy1_5` 与管道日志统一对携带 token 的 `DESKSEND_START`、`DESKSEND_CANCEL`、`DESKSEND_COMMIT_HANDOFF`、`DESKSEND_HANDOFF_ACK`、`DESKSEND_SEND_CANCEL` 做载荷脱敏打印。

### 5.4 对称二段确认与多跳穿透状态机

#### 5.4.1 `Server` 统一跨屏拖拽状态
`Server` 侧由 `FileDropBridge` Coordinator 维护唯一活动跨屏拖拽会话 `ActiveCrossDrag`：
- `phase`: `Idle` | `PreCaptured` | `HandoffPending` | `ActiveCrossScreen`
- `origin`: `Endpoint`（`Primary` 或 `BaseClientProxy*`）
- `originFilesCount`、`originToken`
- `currentTarget`: `Endpoint`（当前光标所在的非发起方屏幕）
- `targetToken`（当前目标的授权口令；当源=目标非同一屏时可复用或生成对应口令）

#### 5.4.2 副机二段确认与防误取消时序
1. **边缘就位与预捕获（不提前破坏 Explorer 拖拽）**：
   - 当 `m_active == Client A` 且 `LMB` 按下时，若指针靠近 `DESKSEND_EDGES` 指定的已连接有效段，`Client A` 激活对应边缘段内的 `DeskflowSentinel`（与跳变区严格对齐，非有效段与锁角区域不激活）。
   - Explorer `DoDragDrop` 进入 `DeskflowSentinel` 并确认 `CF_HDROP` 非空文件列表后（非文件直接返回 `DROPEFFECT_NONE` 透传），`DropAgent` 暂存文件路径，**绝不发送 `VK_ESCAPE` 也不发 `LEFTUP`**，通过核心上报 `DESKSEND_CLIENT_PRECAPTURE:<count>:<edgeMask>`。
   - 若用户尚未触发切屏就移回屏幕内（`DragLeave`），清掉暂存文件并上报 `DESKSEND_CLIENT_PRECAPTURE_CLEAR`，原本地拖拽完好无损。
2. **`Server` 切屏裁决与同步交接**：
   - `Server::isLockedToComputer()` 在检测到 `ActiveCrossDrag` 处于 `PreCaptured`（源为当前 `m_active`）或 `ActiveCrossScreen` 时**返回 `false`**，允许游标参与正常边缘切屏判断。
   - **修复停靠边缘静止不触发问题**：收到 `DESKSEND_CLIENT_PRECAPTURE` 时若左键按下前/瞬间因锁定刚刚把光标钳位在该边且存在越界趋势，按 `mapToNeighbor` + `isSwitchOkay`（包含 `switchCornerSize` 锁角、`switchWaitDelay` / `switchTwoTapDelay` 与邻接在线检查）重新评估一次；未达到切换条件前持续维持正常钳位，**不发送任何取消消息**，不破坏本地拖拽！
   - 一旦 `Server` 判定切换条件满足：将状态置为 `HandoffPending`，记录待切入目标 `pendingTarget` 与入口坐标，并向 `Client A` 下发 `DESKSEND_COMMIT_HANDOFF:<token>`（启动一个短兜底超时，例如 ~150ms，避免客户端异常卡死永远锁屏）。
   - `Client A` 收到 `DESKSEND_COMMIT_HANDOFF` 后：先由拥有 `UIAccess` 的 `deskflow-core` 注入 `VK_ESCAPE` 终止 Explorer `DoDragDrop`、再注入副机本地 `MOUSEEVENTF_LEFTUP` 确保离开后左键不黏连，随即隐藏副机哨兵并回传 `DESKSEND_HANDOFF_ACK:<token>`。
   - `Server` 收到 `DESKSEND_HANDOFF_ACK` 后立刻执行 `switchComputer`（调用 `clientA->leave()` 并 `enter(target)`）：由于 `VK_ESCAPE` 已完成在 `leave()` 把副机光标移至屏幕中心之前，文件绝不会误落在副机中心桌面。

> **主机端对称对齐**：主机（`Primary`）作为源端时同样修正当前 `8px` 哨兵 vs `1px` 切屏区偏移与 `VK_ESCAPE` 时序，只在 `canSwitchFromPrimary()` 允许切换后才提交取消，保证主/副两侧拖拽体验一致。

#### 5.4.3 多跳穿透与结束流转（含 `[副机 A] — [主机] — [副机 B]`）
- **切入首个目标屏（`phase -> ActiveCrossScreen`）**：
  - 若 `target == Primary`：主机侧在 `DropAgent` 显示 `DragHoverIndicator`（跟随胶囊）。注意由于物理 `WM_LBUTTONDOWN` 发生在操控副机期间被钩子吞掉，主机桌面未处于按下态，因此游标掠过主机窗口不会误框选/误点击。
  - 若 `target == Client B`：向 `Client B` 下发 `DESKSEND_START:<token>`，授权 `Client B` 接收器并显示 `DragHoverIndicator`。
- **未松手继续越界切换到下一屏 `nextDst`（多跳穿透）**：
  - `Server::switchComputer` 判断若 `ActiveCrossDrag.phase == ActiveCrossScreen`：
    1) **首先撤销当前中间屏 `currentTarget` 的接收反馈与授权**（`Primary` -> 隐藏跟随胶囊；`Client` -> 下发 `DESKSEND_CANCEL:<token>` 隐去胶囊并 `REVOKE` 口令）；
    2) **若 `nextDst == origin`**（用户把文件拖回起始电脑）：状态彻底回滚为 `Idle`，若 `origin` 为副机则向其下发 `DESKSEND_SEND_CANCEL`，若 `origin` 为主机则清理主机挂起文件；
    3) **若 `nextDst != origin`**（穿过中间屏进入第三台屏幕）：**保持 `origin` 的待发文件不清理**（阻止中间主机在 `onLeavePrimary` 误清空远端拖拽态或覆盖为新拖拽），把 `currentTarget` 更新为 `nextDst` 并在 `nextDst` 启动新一轮 `DESKSEND_START` / 主机悬浮胶囊。
- **在目标屏松开物理左键（`Server::onMouseUp(kButtonLeft)`）**：
  - 先隐藏目标屏 `DragHoverIndicator`：
    - 若 `currentTarget == Primary`：主机 `DropAgent` 写入 `GRANT <token>`、解析 `resolveDropTargetDirectory(m_x, m_y)` 并立即打开右下角 `TransferStatus`“准备接收…”；若 `origin` 是 `Client A`，向 `Client A` 下发 `DESKSEND_SEND_DROP:<m_x>,<m_y>:<tag>:SERVER`。
    - 若 `currentTarget == Client B`：向 `Client B` 下发 `DESKSEND_DROP:<m_x>,<m_y>:<tag>`；若 `origin` 是主机则在主机发起 `localsend-cli send` 到 `Client B` IP；若 `origin` 是 `Client A`，向 `Client A` 下发 `DESKSEND_SEND_DROP:<m_x>,<m_y>:<tag>:<clientBIp>` 直传！
- **异常处理（`Esc` 键取消 / 源端或目标端断线）**：
  - 任意时刻在主机按下 `Esc` 或 `origin` / `currentTarget` 断连（`onClientDisconnected`），`Server` 清理全局态并对存活的源端/目标端分别发送取消通知、隐藏悬浮窗并复位 `Server::isLockedToComputer()`。

### 5.5 接收端两段式视觉反馈交互与实现（位于 `DropAgent`）

1. **光标跟随胶囊（`DragHoverIndicator`）**：
   - **外观与文案**：紧凑深色半透明圆角胶囊（细描边兼顾亮/暗背景），展示简洁图标或指示与中文文案（如 `↓ 松手接收 3 个文件`，文件数为 0/未知时显示 `↓ 松手接收文件`）。
   - **零抢占属性**：Qt `Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus`、`Qt::WA_ShowWithoutActivating`、`Qt::WA_TransparentForMouseEvents`；在创建原生句柄时额外叠加 Win32 `WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW`（以及 `WS_EX_LAYERED` 确保混合渲染穿透点击），绝不影响底层资源管理器文件夹拖放目标高亮或聚焦窗口焦点。
   - **高 DPI 跟随刷新与安全边界**：仅当胶囊处于激活显示态时运行 UI 线程 ~16ms 定时采样（`QCursor::pos()`），配合 `+20, +24` 偏移及按光标所在屏幕 `QGuiApplication::screenAt(pos)->availableGeometry()` 边缘防溢出翻转，确保 125%/150%/200% 多屏缩放下定位准确、靠边不裁切、退出跨屏拖拽即时隐藏。
2. **右下角 `TransferStatus` 连续衔接**：
   - 在接收端收到 `DESKSEND_DROP`（主→副）或主机侧接受远程松手（副→主）的一刻：**同步关闭跟随胶囊**，并立即在接收端打开/置前右下角 `TransferStatus`，文案设为 `"正在准备接收文件..."`（进度条进入忙碌脉冲或等待态）；不再出现从松手到 `RECEIVE_SESSION_STARTED` 之间的空白期。
   - 后续 `RECEIVE_SESSION_STARTED` / `RECEIVE_PROGRESS` 到达时直接无缝刷新进度条、百分比、已传输量、瞬时速率与预计剩余时间；`finalizeActiveSession` 完成后显示 `"已保存到 <目录名>"`（或 `"已复制到剪贴板，按 Ctrl+V 粘贴"`），并在 `~2.5s` 后自动隐退；若用户连续触发下一笔传输则打断旧隐退定时器立即复用刷新。

### 5.6 打包修复：基础卡顿与跨屏拖拽缺陷

1. **输入与链路卡顿修复**：
   - `MSWindowsComputer::warpCursorNoFlush`：移除无谓 `Arch::sleep(0.0)` 让出时间片及冗余 `getThisCursorPos` 轮询检查。
   - `PacketStreamFilter::write`：合并 4 字节长度头与负载为单缓冲一次性调用 `getStream()->write()`，消减短鼠标包分两帧写入造成的 TLS 记录翻倍与 TCP 延迟放大。
   - `ServerProxy::handleMessage` 与 `ClientProxy1_3::resetHeartbeatTimer`：取消客户端每收到任意一条鼠标/业务消息即回发 `kMsgCNoop` 的无效回声放大；重构服务端 `resetHeartbeatTimer()`，避免高频包路径每次调用线性扫描删除并重建 `EventQueueTimer`（改用时间戳脏标记 + 低频单次/周期定时器续期或复用原定时器重设超时）。
   - `MSWindowsDesks::deskMouseRelativeMove`：移除/缓存每次相对移动前后的 4 次 `SystemParametersInfo` 鼠标阈值与加速同步系统调用，优化无桌面切换需求时的同步线程阻塞等待。
2. **拖拽稳定性与坐标精度修复**：
   - `FileDropBridge` 边缘哨兵与过滤修复：使哨兵条与 `getJumpZoneSize()` / 边缘区间严格适配，杜绝“未入切换区先截断”；`SentinelDropTarget::DragEnter` 严格校验非空文件，非 `CF_HDROP` 拖拽直接返回 `DROPEFFECT_NONE` 放行，`onLeftMouseUp` 无论在主屏或副屏释放均保证隐藏哨兵窗口并清理挂起标志。
   - `MSWindowsHook` `VK_LBUTTON` 边缘透传一致化修复，防止边缘按下左键状态丢失或穿透错乱。
   - `MSWindowsDesks::deskMouseMove` 修复：补充 `MOUSEEVENTF_VIRTUALDESK` 并使用全虚拟桌面坐标映射（`SM_XVIRTUALSCREEN` / `SM_YVIRTUALSCREEN` / `SM_CXVIRTUALSCREEN` / `SM_CYVIRTUALSCREEN`）与双精度浮点运算避免归一化 `65535` 精度丢失，并核查修正常规成员初值错误（`MSWindowsDesks.h` 初始值与坐标计算）。
   - `Server.cpp` 修复（此前排查位于 `L1869` 附近）边缘计算偏移 `+ zoneSize` 符号笔误。

---

## 6. 测试与验证计划（Verification & CI）

1. **单元测试（`deskflow-master/src/unittests/`）**：
   - `FileDropBridgeTests.cpp`：
     - 验证 `DESKSEND_EDGES` 精确区间解析与角锁过滤、`DESKSEND_PEERS` 列表合并去重（且活跃传输期间延迟重启接收器）；
     - 验证双向与多跳状态机（`Primary -> Client`、`Client -> Primary`、`Client A -> Primary -> Client B` 多跳穿越时中间节点取消且源端不清空、退回源屏全取消、`Esc`/断线全回滚、非 `CF_HDROP` 不触发预捕获与二段提交未获 `COMMIT_HANDOFF` 不提前发 `VK_ESCAPE`）；
     - 验证 `DESKSEND_DROP` 到来立即转入接收准备状态（不等 `RECEIVE_SESSION_STARTED`）。
   - 协议与基础修复单元测试：
     - `ClientProxy1_5::dragInfoReceived` 完整读取消息负载后，流中紧随其后的下一条消息能正确无错位解析；
     - `Server::isLockedToComputer` 在常规左键按下锁死 vs `PreCaptured` / `ActiveCrossScreen` 解锁行为；
     - `PacketStreamFilter::write` 单次打包写入与解包往返验证；
     - `MSWindowsDesksTests` 多显示器和虚拟桌面坐标映射边界验证。
2. **GitHub Actions CI 构建与测试（`.github/workflows/deskflow-win.yml`）**：
   - 完整 Windows x64 Ninja Release 构建，保证打包二进制与现有全量 `CTest` 全部通过且无回归。
3. **双机实测清单（CI 发布产物下载后验证）**：
   - **安装版默认提权模式（`Service + Elevate = true`）**：确认开机/托盘启动下 `deskflow-core.exe --drop-agent` 自动运行在活跃用户身份，普通 Explorer 拖入副机与从副机拖回主机均不被 UIPI 拦截，且文件名与文件正确落入当前用户桌面/Explorer 当前打开文件夹（非 `systemprofile`）；并验证 UAC 提权弹窗恢复仍正常。
   - **接收端反馈**：主机→副机、副机→主机拖入时均立刻出现光标旁中文胶囊，松手瞬间立即进入准备接收态并显示进度与完成目录。
   - **多跳与干扰排除**：三机串联拖着文件穿过中间屏幕不中断；普通窗口在主/副边缘拖动无卡阻、锁定角落不误丢拖拽、快速移动与不同 DPI（100%/150%）副屏落点准确。

---

## 7. 已做出的工程决策清单（供审阅确认）

以下技术裁决在审阅意见与你选择 **方案 X + 参考推荐建议** 后已直接写入本设计并供核查：
1. **以同二进制 `deskflow-core.exe --drop-agent` + 继承匿名管道实现方案 X**：优于把拖放硬绑死在 `deskflow-gui` 上（即使关掉 GUI 窗口或服务刚开机也稳定可用），且仅用继承匿名管道零全局命名暴露。
2. **高权限合成取消指令仍保留在 `deskflow-core` 内执行**：`--drop-agent` 只负责捕获 OLE `CF_HDROP` 与 UI/文件传输，`VK_ESCAPE` 与局部左键复位仍由拥有 `UIAccess` 的 `deskflow-core` 执行，防止高权限窗口拖拽无法被低权限子进程取消。
3. **采用服务端二段确认替代副机 `DragEnter` 立刻 `VK_ESCAPE`**：配合 `DESKSEND_EDGES` 精确线段区间，彻底解决停靠锁定角落/未达切屏延迟/离线邻居时“手还没切过去、本地拖拽已经被破坏”的问题。
4. **原生支持三机串联不松手多跳穿越（如 `副机 A → 主机 → 副机 B`）**：通过 `ActiveCrossDrag` 分离源端与当前中间目标端状态来实现无损接力。
5. **拖拽期间禁用中继白名单重启 + 打包合入全部卡顿与拖拽基础修复**。
