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

## 2026-10-23：完整宿主内核构建完成，尚未启动验证

编译服务器的 `build-7.1.8-volume90` 与本机已安装内核对应：

- 原 Image SHA-256：
  `5bc08bb63edc67519a2b4a98be5df988fa87de614d92f8fd5079c34fd089c820`；
- 原配置 SHA-256：
  `aa2ab4a8d309f994b4bd32ae8a553527947c7ca87fc5a3c710b25b6f6f5f8852`；
- 原源码提交：`1a157b20dc41d7df9c2b87f4810e113234d0942d`。

任务使用独立目录 `build-7.1.8-himax-a06e05c`，在只读 v7.1.8 共享源码上建立
overlay，用原源码相对基线的 diff 重建已安装版本，并断言重建后的 Git tree 与
原提交相同。原有构建目录、共享源码与本机 kernel-devel 均未修改。随后只应用
本目录的 0001、0002 两项宿主源码补丁。

交叉构建的新 release 为 `7.1.8-gaokun3-himax1`。配置差异为：

- `LOCALVERSION` 使用独立后缀，构建环境设置空 `LOCALVERSION`，避免附加 `+`；
- `INPUT_UINPUT=m`，供当前用户态求解器使用；
- 禁用旧的树内 `TOUCHSCREEN_HIMAX_HX83121A_SPI`，其附属诊断选项随之消失；
- 当前触摸仓库 `a06e05c` 的 V4L2 模块单独针对新内核编译，安装到产物的
  `updates/himax-spi.ko`；`HIMAX_AFE_DIAGNOSTICS=0`，手写笔仍未启用。

完整 `Image modules dtbs`、外置触摸模块和离线 staging 均成功，构建日志未发现
warning/error。最终 staged GPI/SPI/Himax/uinput 模块均为 AArch64，vermagic 一致；
`depmod -e -F System.map` 无未解析符号，`modprobe --show-depends` 能解析唯一的
Himax 模块及依赖。反汇编确认 staged GPI 在 `kfree()` 后写入零指针，GENI
成功分支直接完成 transfer，不再提前写入 message success。

| staged 模块 | Build ID |
|---|---|
| GPI DMA | `b72eb0c756a33d167940119024c41c958215dc06` |
| GENI SPI | `3d725658409dcccc60c0e6cf6d21abd68791af88` |
| Himax V4L2 | `1e02ace5a5c2fef1b1e98f9bdd42eed14634dfc6` |

产物已取回本机，位于被 gitignore 忽略的 `artifacts/host-kernel-a06e05c/`：

- `linux-7.1.8-gaokun3-himax1.tar.zst`：Image、DTB、配置、System.map 与匹配模块；
- `SHA256SUMS`、`staged-modules.txt`、`config.diff`、`host-fixes.diff`；
- 两个修复函数的反汇编、模块依赖检查、构建日志及本次任务的构建/收尾脚本。

归档 SHA-256 为
`f05ccb75e67b66229cf7c8ea3f3a24de373302403d2cdaaaedf0db737f506fb0`，
新 Image SHA-256 为
`a3fe2a4a0de9d839bc845195584e421b3456be12a2e2112cb3ab3bd84446dcb6`。
服务器和本机均已通过清单校验。归档不包含指向编译服务器的 build/source 链接，
也不包含 kernel-devel、initramfs、用户态程序或启动项；它不是可直接刷写的 boot 镜像。

上述构建阶段只构建和取回，没有重载驱动。后续独立启动项部署见下节；
离线构建、符号解析和反汇编均不替代启动与真实手势/睡眠验收。

## 2026-10-23：独立测试启动项已安装，仍运行旧内核

已将新 release 的 Image、配置、System.map、设备树和匹配模块安装到独立版本目录，
并生成 initramfs。采用独立文件安装，没有运行 RPM/kernel-install 钩子，没有删除
旧版本或改变默认启动项，也没有执行 reboot、modprobe、控制器解绑/重绑。

测试项显示名称为 **Fedora Linux 44 (Himax SPI/GPI test)**，版本为
`7.1.8-gaokun3-himax1`。下次由用户在 systemd-boot 菜单中选择此项启动。
当前默认项和正在运行的版本均仍为 `7.1.8-gaokun3+`。

本次新增路径：

- `/usr/lib/modules/7.1.8-gaokun3-himax1/`；
- `/boot/vmlinuz-7.1.8-gaokun3-himax1`、对应 config/System.map/DTB/initramfs；
- ESP 下该 release 的独立 linux/initrd/DTB 目录和一个 BLS `.conf` 文件。

本机 `dkms.service` 已启用，会在开机时使用原有 `/usr/src/himax-spi-0.1` 自动构建。
此次测试包没有新版本的 kernel-devel，且不应让旧源码覆盖已核验的 V4L2 模块，
所以**仅新测试项**追加 `systemd.mask=dkms.service`；没有修改全局服务设置或旧启动项。
当前是手动安装的测试内核，不是已经纳入 RPM/DKMS 管理的正式发布。

部署验证：

- 从新 initramfs 解包 GPI/SPI/Himax/uinput，与新 release 的磁盘模块逐字节比较通过，
  build ID 与上一节一致；nvme、btrfs 等启动依赖也存在；
- ESP 中 linux/initrd/DTB 与源文件比较通过，设备树与旧内核版本完全一致；
- 部署前 ESP 全部 167 个现有文件的 SHA-256 复核通过，原内核和原模块哈希未变；
- 部署前后 bootctl 的 Current Entry、Default Entry 均指向旧版本；
- 当前内核仍为 `7.1.8-gaokun3+`，taint 为 4096（OOT），触摸服务 active、
  NRestarts=0；正在运行的 SPI/Himax build ID 仍为原版本；
- ESP 安装后约剩 68 MiB。未清理任何旧启动文件。

新 initramfs 的 SHA-256 为
`3f59ed80670591e7f96ba0037450695943bf53cc720cc82c0800602cacb69297`。
本机详细部署证据保存在被忽略的 `deploy-logs/host-kernel-install-himax1/`。

启动后仍须确认 `uname -r`、三个修复/触摸模块的运行 build ID、内核故障日志、
raw 帧与触点输出，再验证四边快滑、多指和真实系统睡眠。新项的存在和 initramfs
内容正确不能证明该内核已经成功启动，也不能证明此前整机卡死已在实机消除。

## 2026-10-23：用户重启后的首次验证

用户已手动进入 `7.1.8-gaokun3-himax1`，boot ID 为
`e7887f81-0864-419a-9117-dcb84563b01b`。运行 GPI、GENI SPI、Himax 的 build ID
与构建表格一致；taint=4096（OOT），本次 kernel journal 未见 Oops、坏页、锁死或
Himax SPI 错误。仍有 GPU firmware/audio 等其他子系统警告，不称整个日志无错误。

17:21:18 到 17:22:21 CST 的非中断式服务统计：frames 6853→7985，触点报告
2109→2378，bad=2、stale=0 不变，服务 NRestarts=0。两项坏帧分别为 sync 和
all-zero，无逐帧时间戳，不能确定归属于启动阶段。solver 平均用时 204–210 µs；
IRQ→uinput p99 上界 7 ms，不含扫描前延迟或应用/显示延迟。

该轮没有卸载/替换模块，也没有控制器解绑。当前加载的是 `a06e05c` 触摸构建，
不是后续单 master 读取改动。短期帧流正常不证明休眠唤醒、真实快速入屏手势或
长期稳定性通过。记录：`deploy-logs/host-kernel-install-himax1/boot-validation-20261023.md`。

## 2026-10-23：测试内核更新到触摸提交 `44e6917`

用户要求后，将单次 master 读取模块和新 daemon/replay 安装并加载到同一测试内核。
SPI/GPI 控制器未卸载，其运行 build ID 在前后逐字节核对一致；内核 Image、DTB、
BLS entry、其他内核和默认启动选择不变。新触摸模块 build ID 为
`8373e1e315a0325f115c53592e07a28afd51c94b`，不再是前述 `a06e05c` 模块。

五秒 raw smoke、用户态求解、真实触摸报告与内核健康门控通过；完整统计与测试边界
见 [用户态运行说明](../userspace/README.md)。已重建 `/boot` 与 ESP 测试项 initramfs，
解包确认 Himax/GPI/SPI/uinput 与已安装文件一致。新 initramfs SHA-256 为
`88d55c7559007d53ef23171b187e61a7f78175d671ac1ca4b97a1a7b7f58b17f`。
该镜像尚未重启验证；旧镜像和模块/程序保存在
`deploy-logs/master-read-install-44e6917/previous-*`，不占用 ESP 额外备份空间。
仍是手动部署测试内核，DKMS mask 保留，没有伪造 DKMS 安装状态。
