<div align="center">
  <img src="deskflow-master/src/apps/res/branding/desksend.png" width="104" alt="DeskSend 图标">
  <h1>DeskSend</h1>
  <p><strong>共用一套键鼠，在 Windows、macOS 和 Linux 电脑之间传文件。</strong></p>
  <p>
    <a href="https://github.com/wtyLaziness/desksend-releases/releases/latest"><img src="https://img.shields.io/github/v/release/wtyLaziness/desksend-releases?label=DeskSend" alt="最新正式版本"></a>
    <a href="https://github.com/wtyLaziness/desksend/actions/workflows/deskflow-win.yml"><img src="https://github.com/wtyLaziness/desksend/actions/workflows/deskflow-win.yml/badge.svg" alt="Windows GUI 与核心构建"></a>
    <a href="https://github.com/wtyLaziness/desksend/actions/workflows/localsend-cli-win.yml"><img src="https://github.com/wtyLaziness/desksend/actions/workflows/localsend-cli-win.yml/badge.svg" alt="文件传输组件构建"></a>
  </p>
  <p>
    <a href="https://github.com/wtyLaziness/desksend-releases/releases/latest">下载最新发布包</a> ·
    <a href="docs/使用指南.md">使用指南</a> ·
    <a href="docs/构建与发布.md">构建与发布</a> ·
    <a href="docs/发布与更新.md">更新说明</a> ·
    <a href="LICENSE.md">许可证</a>
  </p>
</div>

DeskSend 将 [Deskflow](https://github.com/deskflow/deskflow) 的键鼠共享与 [LocalSend](https://github.com/localsend/localsend) 的局域网文件传输整合在一个程序中。两端运行 DeskSend 后，就能使用主机的键盘、鼠标操作副机，并把文件或文件夹拖过屏幕边缘发送，无需单独安装或启动 LocalSend。

## 可以做什么

| 能力 | 使用方式 |
| --- | --- |
| 跨屏共享键鼠 | 按实际位置排列主机和副机屏幕，鼠标越过边缘即可切换控制的电脑。 |
| 键鼠网络优先级 | Windows 会为键鼠连接请求更高的网络优先级，缓解与文件传输同时占用 Wi-Fi 时的延迟。 |
| 双向拖文件 | 支持主机 → 副机、副机 → 主机；均使用主机共享的鼠标拖动，源文件保留。 |
| 多机投递 | 多台副机连接同一主机后，可从副机 A 拖着文件穿过主机屏幕，到副机 B 松手；文件由源电脑直传到最终接收电脑。 |
| 拖拽与传输反馈 | 进入目标屏幕后，光标旁显示松手接收提示；松手后显示准备接收、传输进度和结果，传输期间可在任一端进度窗点击「取消传输」。 |
| 按落点保存 | 支持按系统识别桌面或文件管理器落点，其他情况使用默认接收目录和文件剪贴板。 |
| 保留配置更新 | 内置更新器下载并校验完整包，备份待覆盖程序，保留 `settings/` 中的配置、证书和配对。 |

## 快速开始

### 1. 下载完整包

从[最新发布页](https://github.com/wtyLaziness/desksend-releases/releases/latest)下载对应系统的构建包（不同系统电脑之间可直接配合使用同一版本）：

- **Windows x64 便携版**：`DeskSend-<版本>-windows-x64.zip`，完整解压到可写目录后运行 `DeskSend.exe`（不要在压缩包预览中直接运行，也不要只复制 EXE，需保留同目录中的 `deskflow-core.exe`、`localsend-cli.exe`、Qt DLL、插件和 `translations/`）。
- **Windows x64 安装版**：`DeskSend-<版本>-windows-x64.msi`，需要在副机操作 UAC 弹窗或登录界面时安装并开启后台服务及系统身份运行。[UAC 设置步骤](docs/使用指南.md#副机的-uac-弹窗无法点击)
- **macOS arm64（Apple Silicon）**：`DeskSend-<版本>-macos-arm64.tar.gz`，解压出 `DeskSend.app` 后放入“应用程序”或本地可写目录使用（CI 构建采用 ad-hoc 签名；启用跨屏拖拽需在“系统设置 → 隐私与安全性 → 辅助功能”授权）。
- **Linux x64**：`DeskSend-<版本>-linux-x64.tar.gz`，CI 构建归档（包含 `deskflow`、`deskflow-core`、`localsend-cli` 与许可证；需匹配系统的 Qt6/OpenSSL/`libei`/`libportal`/X11 运行库，如环境差异较大可按[构建与发布](docs/构建与发布.md)从源码构建）。

如果打开 Geek Uninstaller 等管理员程序后无法跨屏，请在出现问题的 Windows 电脑上以管理员身份启动 DeskSend。[排查步骤](docs/使用指南.md#打开管理员程序后无法跨屏)

`DeskSend-<版本>-source.zip` 是对应源码归档，不是运行包。

### 2. 连接主机和副机

两台电脑需要处于可互相访问的局域网：

1. **主机（Server，连接实体键鼠）**：选择“使用此计算机的键盘和鼠标”，打开“配置服务器”，添加副机并按实际方位放置。屏幕名称须与副机显示的计算机名一致，然后启动。
2. **副机（Client）**：选择“使用另一台计算机的鼠标和键盘”，填入主机的局域网 IP 地址并启动。不要填写 `127.0.0.1`。
3. 首次 TLS 连接出现证书指纹确认时，核对后信任，再将鼠标移过配置好的边缘检查跨屏操作。

| 入站端口 | 需要放行的电脑 | 用途 |
| --- | --- | --- |
| `TCP 24800` | 主机 | 默认键鼠共享与拖拽控制信令 |
| `TCP 53318` | 所有参与文件接收的主机和副机 | 文件传输；使用双向拖拽时两端均需放行 |

### 3. 发送文件

Windows、macOS 和 Linux 之间可互传文件。

- **跨屏边缘拖拽**支持 Windows、macOS 和 Linux X11 会话。macOS 需在“系统设置 → 隐私与安全性 → 辅助功能”中授权 DeskSend。
- **直传**支持以上三个系统，包括 Linux Wayland。Linux 原生边缘拖放仅在 Qt 使用 `xcb` 后端时启用。连接后，选择“文件 → 发送文件...”可挑选本地文件。也可把本地文件或包含可传输文件的文件夹拖入 DeskSend 主窗口。远程 URL 和已不存在的路径会被忽略。只有一个目标时直接发送，有多个目标时选择接收电脑。支持主机与副机双向发送，也支持连接同一主机的副机之间互传。

使用**主机共享的鼠标**，在支持跨屏拖拽的已连接电脑上拖起文件、文件夹，跨入目标屏幕，在桌面或目标文件夹内松手。副机向主机拖回时也使用同一套共享键鼠。

松手前按 `Esc`，或将文件拖回源电脑，可取消本次跨屏拖拽。传输开始后，鼠标可以移回源电脑继续操作；也可在发送端或接收端的进度窗点击「取消传输」，已收完的文件会保留，未收完的文件会清理。多机投递时，在到达最终目标前保持按住左键，中间屏幕不会接收文件。

| 接收系统 | 跨屏拖拽落点 | 默认目录与剪贴板回退 |
| --- | --- | --- |
| Windows | 资源管理器文件夹或桌面 | 其他窗口和直传默认保存到“下载”，并写入 `CF_HDROP` 文件剪贴板 |
| macOS | 指针坐标命中的 Finder 桌面，或能按窗口边界唯一匹配的 Finder 文件夹窗口 | 其他窗口和直传默认保存到“下载”，并写入系统文件 URL 剪贴板 |
| Linux | X11 会话中识别到的桌面 | 其他窗口、Wayland 或直传默认保存到“下载”，并写入 Qt 文件 URL 和 `x-special/gnome-copied-files` 剪贴板 |

系统未提供“下载”目录路径时，默认接收目录回退到桌面。

三端的接收方都需允许可信局域网入站 TCP 53318。Windows MSI 安装版会添加防火墙规则，便携版需检查程序放行设置。macOS 可在应用防火墙中允许接收程序的入站连接，Linux 可在 UFW 或 firewalld 中按局域网范围放行该端口。

端口检查及异常排查见[使用指南](docs/使用指南.md)。

## 更新与迁移

在“帮助 → 检查 DeskSend 更新…”中下载更新，校验完成后选择“安装并重启”。英文入口为 `Help → Check for DeskSend Updates...`。安装前请等待正在进行的文件传输结束。

手动升级时，先等传输结束并退出旧版，将新完整包解压到新目录，再把旧目录的**整个 `settings/` 文件夹**复制过去，覆盖新包默认配置。更换目录后检查防火墙是否允许新路径。

更新器备份位于 `.desksend-backups/`。更新与回滚机制见[发布与更新说明](docs/发布与更新.md)。

## 当前范围

- 发布页提供 **Windows x64（便携 ZIP 与 MSI 安装版）、macOS arm64（`DeskSend.app` tar.gz，ad-hoc 签名）与 Linux x64（CI tar.gz 归档）**。Windows 副机的 MSI 安装版可启用系统服务，用于操作登录界面和 UAC 安全桌面；自 1.0.9 起，后台服务按需启动，随「退出」停止。内置自动更新目前仅支持 Windows x64。
- 双向及多机拖文件继续使用**主机共享的鼠标**；副机自己的实体鼠标不能独立发起跨屏切换。
- 支持文件与嵌套文件夹；空文件夹和空子目录不会在接收方创建。跨屏发送会保留源文件。
- 主机停止或重启后，保持运行的副机会等待重连。
- 各版变化见[发布记录](https://github.com/wtyLaziness/desksend-releases/releases)。

## 组件与开发

| 路径 / 组件 | 说明 |
| --- | --- |
| [`deskflow-master/`](deskflow-master/) | DeskSend GUI、键鼠核心、跨屏拖拽和更新器；版本由 [`DeskSendVersion.cmake`](deskflow-master/cmake/DeskSendVersion.cmake) 定义。 |
| [`localsend-main/`](localsend-main/) | Rust 文件传输 CLI 和核心协议工作区。 |
| [`scripts/package-release.py`](scripts/package-release.py) | 组装三端发布产物（完整 Windows ZIP/MSI、可选 macOS/Linux 归档）、对应源码与 `SHA256SUMS.txt`。 |
| [`.github/workflows/`](.github/workflows/) | Windows、macOS 和 Linux 的构建、测试及产物。 |
| [`docs/`](docs/) | 使用、构建、发布与更新说明。 |

`DeskSend.exe` 是日常入口；`deskflow-core.exe` 负责键鼠和拖拽控制，`localsend-cli.exe` 在后台传文件。文件字节通过独立的局域网 HTTPS 通道直传，键鼠通道支持 TLS 与证书指纹信任。文件接收按已连接电脑及每次拖拽的临时授权控制。

内部的 `deskflow-core.exe`、`settings/Deskflow.conf` 和协议标识保留兼容名称，请勿手动改名。构建入口见[构建与发布](docs/构建与发布.md)。

## 许可证与致谢

DeskSend 基于 Deskflow 和 LocalSend，保留原作者版权及许可文本：

- **Deskflow 核心、GUI 及对应修改**：`GPL-2.0-only WITH LicenseRef-OpenSSL-Exception`（[GPLv2](deskflow-master/LICENSE)、[OpenSSL 链接例外](deskflow-master/LICENSES/LicenseRef-OpenSSL-Exception.txt)）。
- **LocalSend 组件及对应修改**：`Apache-2.0`（[许可证](localsend-main/LICENSE)）。
- 其他文件和第三方组件按各自声明提供，见[许可证索引](LICENSE.md)。每个公开版本提供对应源码归档。
