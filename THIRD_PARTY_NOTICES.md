# DeskSend 第三方组件许可

| 组件 | 许可证及归属 | 许可与源码入口 |
| --- | --- | --- |
| Deskflow 键鼠共享核心与 GUI | Deskflow Developers、Symless Ltd 及各文件列明的贡献者。GPL-2.0-only，附 OpenSSL 链接例外。 | 随包的 `LICENSE` 或 `Deskflow-LICENSE.txt`、`LICENSE_EXCEPTION` 或 `LICENSES/LicenseRef-OpenSSL-Exception.txt`。DeskSend 修改版的对应源码随同版本发布。 |
| Qt 6 框架及插件 | The Qt Company Ltd. 及 Qt 贡献者。Qt 开源许可选项包括 LGPL-3.0 和 GPL-3.0，个别模块和内嵌组件适用各自的许可。 | [Qt 许可说明](https://doc.qt.io/qt-6/licensing.html) · [Qt 第三方组件](https://doc.qt.io/qt-6/licenses-used-in-qt.html) · [Qt 对应版本源码](https://download.qt.io/archive/qt/) |
| OpenSSL 3.x | OpenSSL Project Authors。Apache License 2.0。 | [OpenSSL 许可](https://openssl-library.org/source/license/) · [OpenSSL 发行源码](https://openssl-library.org/source/) |
| LocalSend Rust CLI 与协议核心 | LocalSend 贡献者。Apache License 2.0。 | 随包的 `LocalSend-LICENSE.txt`。DeskSend 修改版的对应源码位于同版本源码包的 `localsend-main/`。 |

平台包的组件组成随平台变化。Windows 和 macOS 包可包含 Qt 库和插件，macOS 核心使用 OpenSSL 静态链接。Linux tar 依赖目标系统提供 Qt、OpenSSL、libei、libportal 与 X11 运行库等。Rust 依赖的锁定版本见对应源码包内的 `localsend-main/Cargo.lock`，各依赖适用其自身许可。

对应 DeskSend 源码包和版本校验文件可从同一 [DeskSend 发布页](https://github.com/wtyLaziness/desksend-releases/releases) 获取。
