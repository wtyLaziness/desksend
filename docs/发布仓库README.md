# DeskSend

<img src="desksend.png" width="104" alt="DeskSend 图标">

**多台 Windows 电脑共用一套键鼠，文件也能随鼠标双向跨屏拖动。**

DeskSend 整合 Deskflow 键鼠共享与 LocalSend 局域网文件传输。两端运行完整包即可使用，无需单独安装或启动 LocalSend。

[下载最新 Windows x64 完整包](https://github.com/wtyLaziness/desksend-releases/releases/latest) · [源码仓库](https://github.com/wtyLaziness/desksend) · [使用指南](https://github.com/wtyLaziness/desksend/blob/main/docs/使用指南.md) · [更新说明](https://github.com/wtyLaziness/desksend/blob/main/docs/发布与更新.md)

## 主要功能

- **共享键鼠**：按屏幕位置配置主机与副机，使用主机键鼠跨屏操作。
- **双向拖文件**：支持主机 → 副机、副机 → 主机；多台副机连接同一主机时，也可从副机 A 拖过主机屏幕，到副机 B 松手。全程使用主机共享的鼠标，文件由源电脑直传最终接收电脑。
- **落点与反馈**：支持桌面和资源管理器文件夹落点，显示松手接收提示、准备接收、传输进度及结果。落在其他窗口时，保存到默认接收目录并提供文件剪贴板回退。
- **内置更新**：下载完整包并校验 SHA-256，安装前备份待覆盖程序，保留配置、证书和配对。

## 下载与使用

1. 下载 `DeskSend-<版本>-windows-x64.zip`，两端使用同一版本。
2. 将完整目录解压到可写位置，再运行 **`DeskSend.exe`**。不要只复制 EXE，也不要在压缩包预览中直接运行。
3. 主机共享实体键鼠并配置副机屏幕；副机填入主机局域网 IP。首次 TLS 连接时核对证书指纹。
4. 防火墙允许主机入站 **TCP 24800**；使用双向传文件时，**主机和副机均需允许入站 TCP 53318**。

便携包不安装系统服务，不能控制 Windows 登录界面、锁屏或 UAC 安全桌面。副机自己的实体鼠标不能独立触发跨屏切换。嵌套文件夹保留层级，空目录不传输；源文件保留。

## 更新与迁移

使用“帮助 → 检查 DeskSend 更新…”；英文入口为 `Help → Check for DeskSend Updates...`。等待当前文件传输结束后再安装并重启。

手动升级时，先等传输结束并退出旧程序，将新完整包解压到新目录，再将旧目录的**整个 `settings/` 文件夹**复制过去，覆盖新包默认配置。保管好其中的私钥，不要公开上传。

## 源码与许可证

本仓库提供公开版本与更新渠道；开发源码位于 [wtyLaziness/desksend](https://github.com/wtyLaziness/desksend)。每个 Release 附带完整运行包、对应构建源码 `DeskSend-<版本>-source.zip` 和 `SHA256SUMS.txt`。构建提交及文件哈希记录在包内 `desksend-build.json` 中。

Deskflow 核心、GUI 及对应修改使用 `GPL-2.0-only WITH LicenseRef-OpenSSL-Exception`；LocalSend 组件及对应修改使用 `Apache-2.0`。保留原作者版权与许可文本，详见[许可证索引](https://github.com/wtyLaziness/desksend/blob/main/LICENSE.md)。
