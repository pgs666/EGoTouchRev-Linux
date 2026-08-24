# Linux 调参工具

该工具自动查找 Himax SPI 设备的 `algo/` sysfs group，并用 Tk 界面读取或修改驱动
参数。它用于受控 A/B，不负责永久保存 profile。

使用 `uv` 运行：

```sh
cd tuner
uv run tune.py
```

写 sysfs 需要管理员权限。工具会按需请求 `pkexec`，不要用 `sudo` 启动整个桌面
会话。驱动没有加载、sysfs ABI 不匹配或当前会话没有 polkit agent 时，写入会失败。
