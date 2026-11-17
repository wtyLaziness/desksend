# DeskSend 问题排查与反馈

先确认所有电脑使用相同的最新正式版完整包。请从 [DeskSend 发布页](https://github.com/wtyLaziness/desksend-releases/releases/latest)下载，不要用上游 Deskflow 的持续构建包替代 DeskSend。

## 常见问题

- 鼠标无法跨屏：检查主机是否启动、IP 地址和屏幕名称是否正确，主机入站 TCP 24800 是否放行。
- 鼠标可跨屏但文件不传：确认包内有 `localsend-cli.exe`，并放行接收方入站 TCP 53318。双向传输需要两端放行。
- 副机无法拖回主机：使用主机共享的鼠标拖起副机文件，确认返回主机的屏幕边缘已配置且主机允许文件接收端口。
- 文件不在预期位置：检查接收电脑的桌面、目标资源管理器文件夹及“下载”目录；落在其他窗口时可尝试 Ctrl+V。

更多排查步骤见[使用指南](https://github.com/wtyLaziness/desksend/blob/main/docs/使用指南.md)。

## 反馈时提供的信息

可到 [DeskSend Issues](https://github.com/wtyLaziness/desksend/issues)先搜索已有问题。提交反馈时附上：

1. 两端“关于 DeskSend”中复制的版本与系统信息。
2. 主机、副机的屏幕排列、传输方向和复现步骤。
3. 发生时间、完整错误提示，以及发送端和接收端附近的相关日志。

必要时临时开启调试日志，复现后恢复普通日志级别。公开日志前移除私钥、令牌、个人文件路径等敏感信息。

涉及上游 Deskflow 组件的安全问题可参考其[安全说明](Security.md)；不要在公开反馈中附上访问凭据。
