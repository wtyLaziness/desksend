# DeskSend

<img src="deskflow-master/src/apps/res/branding/desksend.png" width="112" alt="DeskSend 图标">

**在两台 Windows 电脑之间共享键盘、鼠标，并把文件拖过屏幕边缘发送到另一台电脑。**

DeskSend 将 Deskflow 的键鼠共享与 LocalSend 的局域网文件传输整合在一个界面和完整便携包中。目前发布 Windows x64 版本。

**[下载最新完整包](https://github.com/wtyLaziness/desksend-releases/releases/latest)** · [详细使用方法](docs/使用指南.md) · [构建与发布](docs/构建与发布.md) · [许可证](LICENSE.md)

## 下载后打开哪个文件

下载发布附件中的 `DeskSend-<版本>-windows-x64.zip`，完整解压后打开 **`DeskSend.exe`**。

请保留同目录的 DLL、插件、翻译文件和 `localsend-cli.exe`。文件传输组件会自动启动，无需单独打开，也不用另外安装 LocalSend。源码 ZIP 面向开发者，日常使用下载 Windows 完整包即可。

## 第一次使用

准备两台处在同一局域网中的 Windows 电脑，并在两端使用同一个版本的完整包。

1. **主机 Server**：连接键盘、鼠标的电脑。打开 DeskSend，选择“使用此计算机的键盘和鼠标”，进入“配置服务器”，添加副机，并把屏幕排在实际相对位置。屏幕名称应与副机界面显示的计算机名一致。
2. **副机 Client**：打开 DeskSend，选择“使用另一台计算机的鼠标和键盘”，填写主机的局域网 IPv4 地址。
3. 启动两端。首次连接如出现证书确认，请核对两端显示的指纹后再信任。
4. 将鼠标移到配置的屏幕边缘，确认可以在两台电脑之间移动和操作。

主机默认接收键鼠连接的端口为 **TCP 24800**，副机接收文件的端口为 **TCP 53318**，需要防火墙允许。详细配置和排查方法见[使用指南](docs/使用指南.md)。

## 怎样传文件

1. 在主机桌面或资源管理器中选中文件或文件夹，按住鼠标左键拖动。
2. 拖向副机所在方向的屏幕边缘，跨屏后在副机桌面或打开的资源管理器文件夹中松手。
3. DeskSend 自动传输，并显示进度和完成或失败状态。主机的源文件保留。

支持多文件和嵌套文件夹。松手前按 Esc 或拖回主机可以取消拖拽；开始传输后可以把鼠标移回主机，文件继续发送。落点不是桌面或资源管理器时，会使用默认接收目录和文件剪贴板回退，详见[文件落点说明](docs/使用指南.md#文件落点)。

## 更新与保留配对

在“帮助 → 检查 DeskSend 更新”中检查版本，下载后选择“安装并重启”。更新包包含键鼠共享和文件传输组件，会校验 SHA-256、备份待覆盖的程序文件，并保留整个 `settings` 目录。

手动换包时，先退出旧程序，将完整包解压到新目录，再把旧目录的整个 **`settings` 文件夹**复制到新目录，覆盖默认设置后运行 `DeskSend.exe`。保留这些文件可继续使用原配置、证书和配对。不要只替换一个 EXE，也不要清空设置来处理普通更新问题。

更新查询需要访问 GitHub；网络不通或接口限流时可以稍后重试，也可从[发布页](https://github.com/wtyLaziness/desksend-releases/releases/latest)下载完整包。

## 当前范围

- 文件拖拽传输方向为 **Server → Client**。
- 空文件夹不会被传输。
- 单次拖动大量文件可能受 Windows 命令行长度限制。
- 便携版没有系统服务，不能控制 Windows 登录界面或 UAC 安全桌面。
- 副机保持运行时会等待主机并尝试重连；主机关闭后无需反复重启副机。

1.0.1 已验证真实双机文件传输与中文界面，并有自动化回归覆盖多文件、嵌套文件夹、32 MiB 文件、拖拽取消、传输中回主机和断线重连。更多电脑和网络环境仍欢迎验证。

## 源码结构

| 路径 | 内容 |
| --- | --- |
| `deskflow-master/` | 键鼠共享核心、DeskSend 界面、跨屏拖拽桥接和更新窗口 |
| `localsend-main/cli/` | 后台文件发送与接收组件 |
| `localsend-main/packages/core/` | LocalSend 协议实现 |
| `phase0/` | 自动化回归、产品检查和完整包组装脚本 |
| `.github/workflows/` | Windows 构建、传输和拖拽回归工作流 |

界面程序、键鼠核心与文件传输组件是不同进程，由 DeskSend 统一启动和更新。部分内部文件名、配置名和协议标识保留 Deskflow 名称，用于兼容已有设置。

## 参与改进

请先阅读[贡献说明](CONTRIBUTING.md)。报告问题时说明版本、主副机角色、复现步骤和错误信息；分享日志前移除证书、密钥、令牌以及不希望公开的地址和文件路径。

## 许可证与致谢

- [Deskflow](https://github.com/deskflow/deskflow) 及相关改动：`GPL-2.0-only WITH LicenseRef-OpenSSL-Exception`。
- [LocalSend](https://github.com/localsend/localsend) 及相关改动：`Apache-2.0`。

其他组件和脚本按各自许可证或 SPDX 标识提供，保留原作者的版权声明。完整说明见[许可证索引](LICENSE.md)。公开发布仓库提供每个版本的对应构建源码、程序包和校验文件。
