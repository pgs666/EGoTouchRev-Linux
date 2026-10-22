# SPI 控制器前置修复

本目录不是 Himax 固件补丁，也不更改 Windows 算法；它保存严格 SPI 传输检查所需的
Linux 宿主控制器修复。触摸模块/DKMS 的构建不会自动修改宿主内核或控制器模块。

## GENI GSI 完成状态与实际长度

`0001-spi-geni-qcom-preserve-in-progress-on-gsi-success.patch` 修复本机
`7.1.8-gaokun3+` 的 `drivers/spi/spi-geni-qcom.c`：GSI DMA 回调在成功时将
`cur_msg->status` 提前设为 0，但 SPI 核心在完成每个 transfer 后要求消息仍为
`-EINPROGRESS`，才能累计 `actual_length` 并继续下一个 transfer。

旧回调会产生“`spi_sync()` 成功、`actual_length=0`”；多 transfer 消息还可能提前
退出。本机首次启用严格长度检查时，第一笔三字节写入就因 `0/3` 被拒绝。
这不是证明芯片实际只收到零字节，而是控制器违反了 SPI 核心的状态/计数约定。
不应在触摸客户端中把所有零长度返回当成成功来规避。

补丁只在 DMA 失败或 residue 非零时设置 `-EIO`，成功时保留消息状态，让 SPI
核心统一累计长度和收尾；也不会清除先前记录的错误。没有改变时钟、DMA 描述符、
CS、Himax 寄存器或重试策略。

适用条件是源码中仍存在上述提前成功赋值；不能仅根据内核版本假定需要或已包含。
已核对的本机原始控制器源文件 SHA-256：

```text
edd0d278011d2200b9a2ee84b4ea923ed4605b4b61f25997c15e8d494348af61
```

在独立的、与目标内核配置相符的源码工作树中检查/应用补丁，再由对应内核工程构建
控制器模块或完整内核。例如在源码根目录执行：

```sh
git apply --check /path/to/EGoTouchRev-Linux/kernel-patches/0001-spi-geni-qcom-preserve-in-progress-on-gsi-success.patch
git apply /path/to/EGoTouchRev-Linux/kernel-patches/0001-spi-geni-qcom-preserve-in-progress-on-gsi-success.patch
```

不要直接改只读共享基线或已安装的 kernel-devel 源码；不要未经检查就卸载一个同时
连接其他设备的 SPI 控制器。替换模块必须核对目标 kernel release/vermagic，保留
原模块和 initramfs 恢复手段。更换内核后需要重新提供该内核对应的修复版本，不能
跨内核复用 `.ko`。本仓库不自动修改另一个内核/buildbot 仓库。

## 测试边界

`geni-completion-policy` 对保留的原始回调 fixture 应用实际补丁，并编译执行测试。
覆盖零长度成功复现、双 transfer 长度累计、DMA error/residue 和已有错误保留，
测试自身使用 ASan/UBSan。SPI 核心部分是状态门控模型，不是完整内核或 DMA 模拟器。
其他 native SPI 对照测试继续拒绝零长度成功，避免把控制器缺陷固化为客户端行为。

真实模块试载和部署记录见 [用户态说明](../userspace/README.md) 的时间顺序记录。
成功的单 transfer Himax 采集不能替代其他 SPI 设备、多 transfer 消息和真实 DMA
故障注入验证。
