# 参与贡献 DeskSend

感谢关注并参与改进 DeskSend。提交 Issue、代码改动或文档补充前，请先浏览 [README](README.md)、[详细使用指南](docs/使用指南.md)、[构建与发布](docs/构建与发布.md) 与 [许可证索引](LICENSE.md)。

---

## 目录分区与工具链要求

DeskSend 仓库由多个相对独立的子模块组成，请按修改位置准备对应工具链：

| 路径 | 技术栈 | 工具链 / 版本要求 | 对应许可证 |
| --- | --- | --- | --- |
| [`deskflow-master/`](deskflow-master/) | C++20 / Qt 6 | MSVC x64、CMake、Ninja、Qt `6.8.*`、vcpkg | `GPL-2.0-only WITH LicenseRef-OpenSSL-Exception` |
| [`localsend-main/`](localsend-main/) | Rust (Cargo Workspace：`cli/` + `packages/core/`) | [`localsend-main/rust-toolchain.toml`](localsend-main/rust-toolchain.toml) 指定工具链（`1.97.1` + `clippy`） | `Apache-2.0` |
| [`scripts/`](scripts/) 与 [`tests/`](tests/) | Python / PowerShell | PowerShell 7（`pwsh -STA`）、Python 3 | 见各文件 SPDX 头（如 `MIT`） |
| [`docs/`](docs/) 与根目录文档 | Markdown | — | 与所属项目一致 |

---

## 本地构建与测试验证

修改任意代码后，请在提交前针对受影响模块完成实际编译与测试。

### 1. C++ / Qt 部分（`deskflow-master/`）

配置与构建示例（Qt 与 vcpkg 路径替换为本机路径）：

```powershell
$qtDir = 'C:/Qt/6.8.3/msvc2022_64'
$vcpkgDir = 'C:/src/vcpkg'
$env:PATH = "$qtDir/bin;$env:PATH"

cmake -S deskflow-master -B deskflow-master/build -G Ninja `
  -DCMAKE_BUILD_TYPE=Release -DBUILD_INSTALLER=ON -DBUILD_TESTS=ON `
  -DCMAKE_PREFIX_PATH="$qtDir" `
  -DCMAKE_TOOLCHAIN_FILE="$vcpkgDir/scripts/buildsystems/vcpkg.cmake"

cmake --build deskflow-master/build
```

运行全量或定向 C++ 单元测试（注意测试根目录位于 `build/src/unittests`）：

```powershell
# 全量单元测试
ctest --test-dir deskflow-master/build/src/unittests --output-on-failure

# 定向复测受影响模块（示例）
ctest --test-dir deskflow-master/build/src/unittests -R "FileDropBridgeTests|VersionCheckerTests" --output-on-failure
```

### 2. Rust 部分（`localsend-main/`）

```powershell
Push-Location localsend-main
rustup toolchain install
cargo test --release --package localsend --package localsend-cli
cargo build --release --package localsend-cli
Pop-Location
```

若改动了核心协议 crate，额外验证全部特性组合，并保持 `localsend-main/Cargo.toml` 现有 workspace 结构不变：

```powershell
Push-Location localsend-main/packages/core
cargo test --features full
Pop-Location
```

### 3. 更新器、品牌校验与端到端回环（`tests/`）

- **便携更新器自测**（覆盖正常安装、过期文件清理与备份修剪、外层/内层哈希错误、缺失组件、Zip-Slip 路径穿越共 6 项用例，不会改动现有工作区设置）：

  ```powershell
  pwsh -NoProfile -File tests/update-test.ps1
  ```

- **品牌与可执行身份检查**：

  ```powershell
  pwsh -NoProfile -File tests/branding-test.ps1 -DeskflowDir <解压后的便携包目录> -OutDir <临时输出目录>
  ```

- **完整跨屏拖拽回环 E2E**（需要桌面交互会话、`pwsh -STA`、基础 Deskflow 解压目录与 `localsend-cli.exe`）：

  ```powershell
  # 常规拖放
  pwsh -STA -NoProfile -File tests/loopback-test.ps1 -DeskflowDir <目录> -LocalSendCliExe <localsend-cli.exe> -OutDir <临时目录> -DisableLock "false"

  # mTLS 连接 + 断线重连拖放
  pwsh -STA -NoProfile -File tests/loopback-test.ps1 -DeskflowDir <目录> -LocalSendCliExe <localsend-cli.exe> -OutDir <临时目录> -DisableLock "false" -TlsEnabled

  # Esc 取消 / 拖回主屏松手取消
  pwsh -STA -NoProfile -File tests/loopback-test.ps1 -DeskflowDir <目录> -LocalSendCliExe <localsend-cli.exe> -OutDir <临时目录> -DisableLock "false" -CancelDrag
  pwsh -STA -NoProfile -File tests/loopback-test.ps1 -DeskflowDir <目录> -LocalSendCliExe <localsend-cli.exe> -OutDir <临时目录> -DisableLock "false" -ReturnBeforeRelease
  ```

> `.gitignore` 已忽略 `deskflow-master/build/`、`deskflow-bin/`、`tests/bin/` 与 `localsend-main/target/`；提交前仍应检查并清理额外的临时目录或 Python `__pycache__`。

---

## 核心工程约束（避坑指南）

为避免破坏跨组件协作、安全边界与存量兼容，修改代码时请遵守以下约束：

1. **禁止明文暴露传输凭据与大量路径**：
   - `FileDropBridge` 启动 `localsend-cli` 时通过标准输入（`--auth-stdin`、`--pin-stdin`、`--paths-stdin`）传递一次性令牌和路径列表，不得改回命令行传参。
   - 控制协议日志与 `RECEIVE_SESSION_STARTED` 等标准输出事件仅记录 16 位 SHA-256 hex `pin_tag`，禁止在日志中打印原始 PIN 或令牌。
   - `DESKSEND_CANCEL:<tag>` 必须严格按 `pin_tag` 关联或清理等待队列，避免误取消并发或后续传输。

2. **保留上游兼容性命名与路径**：
   - `deskflow-core.exe`、`settings/Deskflow.conf`（便携模式检测依据）、`settings/tls/` 与 `kOrgDomain`（`deskflow.org`）等底层标识关联现有配置、证书与图标资源，不要随意重命名。

3. **单元测试与时钟/平台边界**：
   - `FileDropBridge` 中涉及弹窗或状态展示的逻辑，必须先检查 `QApplication::instance()` / UI 上下文，保证在 `QCoreApplication` 无头单测中安全退化。
   - 超时清理与去抖优先使用 `std::chrono::steady_clock`，做差前须先比较大小以防无符号整型下溢。
   - Windows 上 `std::time::SystemTime` 具有 100 ns `FILETIME` 精度限制，涉及纳秒时间戳断言的 Rust 测试需区分 `#[cfg(windows)]`。

4. **界面文案与国际化**：
   - `MainWindow`、`UpdateDialog`、`VersionChecker`、`FileDropBridge` 等模块新增或修改的用户可见文案须包裹 `tr(...)` 或 `QCoreApplication::translate(...)`，并同步维护 [`deskflow-master/translations/deskflow_zh_CN.ts`](deskflow-master/translations/deskflow_zh_CN.ts)。

5. **版权、SPDX 与最小改动**：
   - 保留上游源码注释、版权声明与文件级 SPDX 标识；修改保持聚焦，不顺带重构无关模块。只修改文档时无需重编二进制，但需核对文内命令、路径与源码实情一致。

---

## CI 工作流与 Pull Request 流程

GitHub Actions 包含以下主要工作流：

| 工作流 | 触发范围 | 验证内容 |
| --- | --- | --- |
| [`deskflow-win`](.github/workflows/deskflow-win.yml) | `deskflow-master/**`、`tests/update-test.ps1` | MSVC x64 + Qt 6.8 构建、打包 ZIP、全部 CTest、以及 `tests/update-test.ps1`。 |
| [`localsend-cli-win`](.github/workflows/localsend-cli-win.yml) | `localsend-main/cli/**`、`localsend-main/packages/core/**` 与 Rust 清单 | Rust 单元与集成测试、`localsend-cli.exe` 发布构建、以及 32 MiB 同机与 `--auth-stdin` 收发 E2E。 |
| [`e2e-loopback`](.github/workflows/e2e-loopback.yml) | 手动触发（`workflow_dispatch`） | 品牌烟测、常规跨屏传输、mTLS 连接/重连传输、`Esc` 取消与回主屏取消 E2E。 |

> 仅修改 `tests/` 测试或代理脚本而未改动生产源码时，触发 `e2e-loopback` 可将输入参数 `binaries_commit` 指向生产源码完全相同的已有构建提交，工作流会自动校验 `git diff` 并复用其产物。

### 提交前检查清单

- [ ] 改动围绕单一目标，未包含无关格式化或废弃文件。
- [ ] 已运行与修改相匹配的本地测试或 GitHub Actions 工作流；文档类改动已核对链接和命令行可用性。
- [ ] 使用独立临时目录和虚构数据进行测试，不读取个人真实资料或生产证书。
- [ ] 确认 `git status` 不含运行目录、构建输出、`settings/`、`deskflow-bin/`、`tests/bin/`、`*.pem`、`*.key`、`*.log` 或任何密钥/token。
- [ ] 提交说明和 PR 描述中写清背景、行为变化以及**实际执行过**的验证方式与范围。

---

## 问题反馈规范（Issue）

提交故障报告时，请尽量提供以下信息（使用虚构文件名与示例 IP）：

1. **版本信息**：DeskSend 版本（如 `1.0.1`）及 Windows 版本。
2. **角色与环境**：出现问题的电脑属于主机（Server）还是副机（Client），是否开启 TLS 加密。
3. **复现步骤**：从选文件、拖拽跨屏到松手的具体操作，以及期望结果与实际表现。
4. **排障信息**：
   - 连接超时问题：附上 `Test-NetConnection <目标IP> -Port 24800` 与 `53318` 的检测结果。
   - 拖拽/传输问题：附上脱敏后的 `FileDropBridge` 前后关键日志片段。

**分享前务必脱敏**：移除私钥、证书文件、密码、授权令牌、真实个人文件路径与不希望公开的局域网/公网地址；切勿打包上传整个运行目录或 `settings/` 文件夹。

---

## 发布约定摘要

维护者制作公开版本时须遵循 [构建与发布](docs/构建与发布.md) 与 [发布与更新说明](docs/发布与更新.md)：

- 完整便携包 `DeskSend-<版本>-windows-x64.zip` 必须同时包含 `DeskSend.exe`、`deskflow-core.exe`、`localsend-cli.exe`、清单 `desksend-build.json` 与各自许可证文本（不得将缺少文件传输组件的基础 Deskflow ZIP 作为完整版分发）。
- 使用 `scripts/package-release.py` 从同一构建提交组装程序包、`DeskSend-<版本>-source.zip` 与 `SHA256SUMS.txt`，确保发布不混入本地 `settings/` 或私钥日志。
- 双仓库（`wtyLaziness/desksend` 与 `wtyLaziness/desksend-releases`）的正式 Release 标签保持 `v<版本>` 一致，且严禁将 GitHub token 写入仓库、代码或构建产物。
