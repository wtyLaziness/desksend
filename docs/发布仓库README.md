# DeskSend

<img src="desksend.png" width="104" alt="DeskSend 图标">

**多台 Windows 电脑共用一套键鼠，文件也能随鼠标双向跨屏拖动。**

DeskSend 整合 Deskflow 键鼠共享与 LocalSend 局域网文件传输。两端运行完整包即可使用，无需单独安装或启动 LocalSend。

[下载最新 Windows x64 完整包](https://github.com/wtyLaziness/desksend-releases/releases/latest) · [源码仓库](https://github.com/wtyLaziness/desksend) · [使用指南](https://github.com/wtyLaziness/desksend/blob/main/docs/使用指南.md) · [更新说明](https://github.com/wtyLaziness/desksend/blob/main/docs/发布与更新.md)

## 主要功能

- **共享键鼠**：按屏幕位置配置主机与副机，使用主机键鼠跨屏操作。
- **键鼠网络优先级**：Windows 会为键鼠连接请求更高优先级，文件传输无固定速率上限。
- **双向拖文件**：支持主机 → 副机、副机 → 主机；多台副机连接同一主机时，也可从副机 A 拖过主机屏幕，到副机 B 松手。全程使用主机共享的鼠标，文件由源电脑直传最终接收电脑。
- **落点与反馈**：支持桌面和资源管理器文件夹落点，显示松手接收提示、准备接收、传输进度及结果；松手前可按 `Esc` 或拖回源屏取消，传输期间可在任一端进度窗点击「取消传输」。落在其他窗口时，保存到默认接收目录并提供文件剪贴板回退。
- **内置更新**：下载完整包并校验 SHA-256，安装前备份待覆盖程序，保留配置、证书和配对。

## 下载与使用

1. 下载 `DeskSend-<版本>-windows-x64.zip`，两端使用同一版本。
2. 将完整目录解压到可写位置，再运行 **`DeskSend.exe`**。不要只复制 EXE，也不要在压缩包预览中直接运行。
3. 主机共享实体键鼠并配置副机屏幕；副机填入主机局域网 IP。首次 TLS 连接时核对证书指纹。
4. 防火墙允许主机入站 **TCP 24800**；使用双向传文件时，**主机和副机均需允许入站 TCP 53318**。

需要从主机操作副机的 UAC 弹窗时，在副机安装 `DeskSend-<版本>-windows-x64.msi`，并开启后台服务和系统身份运行。[UAC 设置步骤](https://github.com/wtyLaziness/desksend/blob/main/docs/使用指南.md#副机的-uac-弹窗无法点击)

如果打开 Geek Uninstaller 等管理员程序后无法跨屏，请在出现问题的电脑上以管理员身份启动 DeskSend。[排查步骤](https://github.com/wtyLaziness/desksend/blob/main/docs/使用指南.md#打开管理员程序后无法跨屏)

自 1.0.9 起，Windows 服务随键鼠共享按需启动。选择托盘菜单中的“退出”会停止后台服务。

副机自己的实体鼠标不能独立触发跨屏切换。嵌套文件夹保留层级，空目录不传输；源文件保留。

## 更新与迁移

使用“帮助 → 检查 DeskSend 更新…”；英文入口为 `Help → Check for DeskSend Updates...`。等待当前文件传输结束后再安装并重启。

便携版与 MSI 安装版均支持在应用内检查、下载并校验 SHA-256 更新包；安装版在点击“安装并重启”后会调用 `msiexec` 升级（也可直接下载新版 MSI 安装）。

手动升级时，先等传输结束并退出旧程序，将新完整包解压到新目录，再将旧目录的**整个 `settings/` 文件夹**复制过去，覆盖新包默认配置。保管好其中的私钥，不要公开上传。

## 源码与许可证

本仓库提供公开版本与更新渠道；开发源码位于 [wtyLaziness/desksend](https://github.com/wtyLaziness/desksend)。自 1.0.8 起，Release 提供完整便携 ZIP、MSI 安装包、对应构建源码 `DeskSend-<版本>-source.zip` 和 `SHA256SUMS.txt`。便携包的构建提交及文件哈希记录在 `desksend-build.json` 中。

Deskflow 核心、GUI 及对应修改使用 `GPL-2.0-only WITH LicenseRef-OpenSSL-Exception`；LocalSend 组件及对应修改使用 `Apache-2.0`。保留原作者版权与许可文本，详见[许可证索引](https://github.com/wtyLaziness/desksend/blob/main/LICENSE.md)。
