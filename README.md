<div align="center">
  <img src="deskflow-master/src/apps/res/branding/desksend.png" width="104" alt="DeskSend 图标">
  <h1>DeskSend</h1>
  <p><strong>多台 Windows 电脑共用一套键鼠，文件也能随鼠标跨屏拖过去。</strong></p>
  <p>
    <a href="https://github.com/wtyLaziness/desksend-releases/releases/latest"><img src="https://img.shields.io/github/v/release/wtyLaziness/desksend-releases?label=DeskSend" alt="最新正式版本"></a>
    <a href="https://github.com/wtyLaziness/desksend/actions/workflows/deskflow-win.yml"><img src="https://github.com/wtyLaziness/desksend/actions/workflows/deskflow-win.yml/badge.svg" alt="Windows GUI 与核心构建"></a>
    <a href="https://github.com/wtyLaziness/desksend/actions/workflows/localsend-cli-win.yml"><img src="https://github.com/wtyLaziness/desksend/actions/workflows/localsend-cli-win.yml/badge.svg" alt="文件传输组件构建"></a>
  </p>
  <p>
    <a href="https://github.com/wtyLaziness/desksend-releases/releases/latest">下载 Windows x64 完整包</a> ·
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
| 拖拽与传输反馈 | 进入目标屏幕后，光标旁显示松手接收提示；松手后显示准备接收、传输进度和结果。 |
| 按落点保存 | 接收方桌面或资源管理器文件夹可作为落点；其他窗口回退到默认接收目录，并提供文件剪贴板供 `Ctrl+V` 粘贴。 |
| 保留配置更新 | 内置更新器下载并校验完整包，备份待覆盖程序，保留 `settings/` 中的配置、证书和配对。 |

## 快速开始

### 1. 下载完整包

从[最新发布页](https://github.com/wtyLaziness/desksend-releases/releases/latest)下载 `DeskSend-<版本>-windows-x64.zip`。两台电脑使用同一版本，完整解压到可写目录后运行 `DeskSend.exe`。

需要操作副机 UAC 弹窗时，在副机使用同一发布页的 `DeskSend-<版本>-windows-x64.msi` 安装版，并开启后台服务及系统身份运行。[UAC 设置步骤](docs/使用指南.md#副机的-uac-弹窗无法点击)

不要在压缩包预览中直接运行，也不要只复制 EXE。请保留同目录中的 `deskflow-core.exe`、`localsend-cli.exe`、Qt DLL、插件和 `translations/`。`DeskSend-<版本>-source.zip` 是源码归档，不是运行包。

### 2. 连接主机和副机

两台电脑需要处于可互相访问的局域网：

1. **主机（Server，连接实体键鼠）**：选择“使用此计算机的键盘和鼠标”，打开“配置服务器”，添加副机并按实际方位放置。屏幕名称须与副机显示的计算机名一致，然后启动。
2. **副机（Client）**：选择“使用另一台计算机的鼠标和键盘”，填入主机的局域网 IP 地址并启动。不要填写 `127.0.0.1`。
3. 首次 TLS 连接出现证书指纹确认时，核对后信任，再将鼠标移过配置好的边缘检查跨屏操作。

| 入站端口 | 需要放行的电脑 | 用途 |
| --- | --- | --- |
| `TCP 24800` | 主机 | 默认键鼠共享与拖拽控制信令 |
| `TCP 53318` | 所有参与文件接收的主机和副机 | 文件传输；使用双向拖拽时两端均需放行 |

### 3. 拖动文件

使用**主机共享的鼠标**，在任一已连接电脑的桌面或资源管理器中拖起文件、文件夹，跨入目标屏幕，在桌面或目标文件夹内松手。副机向主机拖回时也使用同一套共享键鼠。

松手前按 `Esc`，或将文件拖回源电脑，可取消本次跨屏拖拽。传输开始后，鼠标可以移回源电脑继续操作。多机投递时，在到达最终目标前保持按住左键，中间屏幕不会接收文件。

文件落点、端口检查及异常排查见[使用指南](docs/使用指南.md)。

## 更新与迁移

在“帮助 → 检查 DeskSend 更新…”中下载更新，校验完成后选择“安装并重启”。英文入口为 `Help → Check for DeskSend Updates...`。安装前请等待正在进行的文件传输结束。

手动升级时，先等传输结束并退出旧版，将新完整包解压到新目录，再把旧目录的**整个 `settings/` 文件夹**复制过去，覆盖新包默认配置。更换目录后检查防火墙是否允许新路径。

更新器备份位于 `.desksend-backups/`。更新与回滚机制见[发布与更新说明](docs/发布与更新.md)。

## 当前范围

- 提供 **Windows x64 便携版与 MSI 安装版**。副机的 MSI 安装版可启用系统服务，用于操作 Windows 登录界面和 UAC 安全桌面。自 1.0.9 起，后台服务按需启动，随「退出」停止。
- 双向及多机拖文件继续使用**主机共享的鼠标**；副机自己的实体鼠标不能独立发起跨屏切换。
- 支持文件与嵌套文件夹；空文件夹和空子目录不会在接收方创建。跨屏发送会保留源文件。
- 主机停止或重启后，保持运行的副机会等待重连。
- 各版变化见[发布记录](https://github.com/wtyLaziness/desksend-releases/releases)。

## 组件与开发

| 路径 / 组件 | 说明 |
| --- | --- |
| [`deskflow-master/`](deskflow-master/) | DeskSend GUI、键鼠核心、跨屏拖拽和更新器；版本由 [`DeskSendVersion.cmake`](deskflow-master/cmake/DeskSendVersion.cmake) 定义。 |
| [`localsend-main/`](localsend-main/) | Rust 文件传输 CLI 和核心协议工作区。 |
| [`scripts/package-release.py`](scripts/package-release.py) | 组装完整 Windows 包、对应源码和 `SHA256SUMS.txt`。 |
| [`.github/workflows/`](.github/workflows/) | Windows GUI/核心与 Rust CLI 的构建、测试及产物。 |
| [`docs/`](docs/) | 使用、构建、发布与更新说明。 |

`DeskSend.exe` 是日常入口；`deskflow-core.exe` 负责键鼠和拖拽控制，`localsend-cli.exe` 在后台传文件。文件字节通过独立的局域网 HTTPS 通道直传，键鼠通道支持 TLS 与证书指纹信任。文件接收按已连接电脑及每次拖拽的临时授权控制。

内部的 `deskflow-core.exe`、`settings/Deskflow.conf` 和协议标识保留兼容名称，请勿手动改名。构建入口见[构建与发布](docs/构建与发布.md)。

## 许可证与致谢

DeskSend 基于 Deskflow 和 LocalSend，保留原作者版权及许可文本：

- **Deskflow 核心、GUI 及对应修改**：`GPL-2.0-only WITH LicenseRef-OpenSSL-Exception`（[GPLv2](deskflow-master/LICENSE)、[OpenSSL 链接例外](deskflow-master/LICENSES/LicenseRef-OpenSSL-Exception.txt)）。
- **LocalSend 组件及对应修改**：`Apache-2.0`（[许可证](localsend-main/LICENSE)）。
- 其他文件和第三方组件按各自声明提供，见[许可证索引](LICENSE.md)。每个公开版本提供对应源码归档。
