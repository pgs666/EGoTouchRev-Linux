# SPI 控制器前置修复

本目录不是 Himax 固件补丁，也不更改 Windows 算法；它保存严格 SPI 传输检查所需的
Linux 宿主控制器修复。触摸模块/DKMS 的构建不会自动修改宿主内核或控制器模块。

**2026-10-23 实机事故：当前内核不能安全热卸载/重载 GENI SPI 控制器。**
此前单独替换 `spi-geni-qcom.ko` 的试验发生了内核内存破坏；raw 帧采集和脚本返回
成功不构成安全通过。不要重跑旧的 controller trial/install 脚本。
先将以下两项修复纳入匹配的内核工程，通过重启进入修复内核后验证；不要热替换
共享 GPI 驱动。当前运行的生产模块未随本次源码修复更新。

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

不要直接改只读共享基线或已安装的 kernel-devel 源码；即使 SPI 控制器只有一个
从设备，也不能据此推断可安全卸载。构建必须核对目标 kernel release/vermagic，保留
原模块和 initramfs 恢复手段。更换内核后需要重新提供该内核对应的修复版本，不能
跨内核复用 `.ko`。本仓库不自动修改另一个内核/buildbot 仓库。

## GPI DMA 配置的释放后使用

`0002-dmaengine-qcom-gpi-clear-freed-peripheral-config.patch` 修复本机 GPI DMA
通道的配置生命周期。原 `gpi_free_chan_resources()` 释放 `gchan->config` 后未清空
指针；重新申请同一通道时，`gpi_peripheral_config()` 把悬空指针传入 `krealloc()`，
再复制新配置。通道对象在 SPI 控制器卸载后仍由 GPI 驱动保存。

这是原 DMA 驱动已有的生命周期缺陷，热替换控制器触发了它；不是证据表明补丁
0001 的 `actual_length` 逻辑本身产生了野指针。换回原控制器也会再次释放/申请通道，
无法修复已经损坏的内核内存。

本机原 GPI 源码 SHA-256：

```text
a89b8e2e2e3b4f38d607700d461f814affc4c2614713c951c14388f273499a5a
```

2026-10-23 06:09:22，换入实验控制器约两秒后，DRM
`drm_atomic_state_default_clear()` 出现首次 Oops；06:10:13 回退原控制器时，
`__memcg_kmem_charge_page()` 出现第二次 Oops。两处坏指针基值都是
`0x0000040101010001`，与本机 SPI 配置的前八字节
`01 00 01 01 01 04 00 00` 一致（set_config、loopback、CPOL、CPHA、pack、
word_len、fragmentation、CS）。本机已安装 `gpi.ko` 的反汇编也确认释放后没有
清空该字段。这为内存覆写来源提供了很强证据，但没有内存转储，不能声称排除所有
其他缺陷或证明每次强制重启的完整路径。

补丁在 `kfree()` 后清空指针；修复源码不等于已部署、也不等于 DMA 并发/硬件验证。
修复前不要卸载控制器或共享 GPI 驱动；下一阶段需在编译服务器的独立内核工作树
集成两项修复，重启验证后再考虑通道释放/重用实测。

## 测试边界

`geni-completion-policy` 对保留的原始回调 fixture 应用实际补丁，并编译执行测试。
覆盖零长度成功复现、双 transfer 长度累计、DMA error/residue 和已有错误保留，
测试自身使用 ASan/UBSan。SPI 核心部分是状态门控模型，不是完整内核或 DMA 模拟器。
其他 native SPI 对照测试继续拒绝零长度成功，避免把控制器缺陷固化为客户端行为。

`gpi-channel-lifetime` 保留本机未经修改的两段 GPI 函数（fixture 有哈希保护），
用 host 分配器复现释放后重新配置的 ASan double-free，并对实际补丁做零 fuzz 应用。
修复后覆盖 100 次双通道释放/重新配置、不同长度、无效配置、分配失败和重复释放。
这个测试验证所有权，不模拟 slab、DMA、IRQ、任务并发或真实内核卸载。

部署脚本新增基于 journal cursor 与 taint 的检查，已有 Oops/坏页时拒绝硬件操作，
新 Oops/WARN/坏页或 journal 读取失败不记为通过。检测到内核故障后停止自动
热回退，保留 checkpoint；回退模块无法恢复内存。此检查不是 KASAN 或故障转储，
不能证明没有静默内存破坏。

真实模块试载和部署记录见 [用户态说明](../userspace/README.md) 的时间顺序记录。
成功的单 transfer Himax 采集不能替代其他 SPI 设备、多 transfer 消息和真实 DMA
故障注入验证。
