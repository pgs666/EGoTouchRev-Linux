# Windows SPB 内核后端审计（2026-10-23）

## 目的与结论

审计默认用户态移植尚未覆盖的 `SpiModule/HalDevice → SpbThpTool.sys` 边界。
本文是静态证据，不是该 Windows 驱动的运行记录，也不宣称 Linux 已逐项等价。
主动笔仍按用户要求关闭；没有为了这次审计发送 `F4/F5` 命令。

已确认：

- Windows 内核后端使用 ACPI 资源和 KMDF Resource Hub，并非只有固定名称的
  “无 ACPI 绑定测试工具”。两个用户态名字不能证明两个物理 SPI 片选。
- Windows `GetFrame` 从内核软件 FIFO 取数据，不直接发出事件栈 SPI 读取。
- IRQ 回调及 `IntOpen` 发起整帧 SPI 读取，完成回调负责入队；这是一个独立于
  用户态求解的生产者。
- 此版本 `IntOpen` 是空闲时补发一次读取，忙时不重复提交；没有硬件 reset。
  同版本 `IntClose` 的分发路径只返回成功，没有对应的禁用 IRQ 操作。
- Linux 单次 master 事务长度已对齐，但停帧恢复、清帧、队列策略仍不完全等价。
  尤其不能用重启 daemon/重新初始化 AFE 替代 Windows 的无复位补读。

## 输入与复核方法

Windows 分区 `/dev/nvme0n1p3` 以 `ntfs-3g -o ro,norecover` 挂载，复制后已卸载。
未写入 Windows 分区，未执行 Windows 二进制。输入为：

| 项目 | 值 |
|---|---|
| 二进制 | `Windows/System32/drivers/SpbThpTool.sys` |
| SHA-256 | `203bbf7d470dd1f961ee4687082d0d9a7bd17304d2d57705137594b07fc2f8b5` |
| 大小、架构 | 57416 字节，ARM64 PE32+ |
| DriverStore | `spbthptool.inf_arm64_f56d385517546dd1`，其中 `.sys` 与 System32 副本哈希相同 |
| INF DriverVer | `09/07/2022,9.46.56.200` |
| INF model line | `%SpbThpTool.DeviceDesc%=SpbThpTool_Device, ACPI\HIMX0001, ACPI\HIMX0002` |
| PDB 路径字符串 | `D:\GAOKUN\SpbTestTool\sys\ARM64\Release\SpbThpTool.pdb`，未取得 PDB |
| 上游用户态引用 | EGoTouchRev v1.4.0，`4d4f53d0bd8563d20850d5f380bcd6a8704c1e81` |

Ghidra 12.1.3 导入 `AARCH64:LE:64:v8A:windows`，90 个函数反编译成功；对关键
分支另导出 ARM64 指令，并直接读取 PE literal pool，避免将反编译器生成的
`DAT_...` 当成已经识别的常量。地址均为该输入的静态 VA（image base `0x140000000`）。

私有输入、完整反编译和指令清单仅在被忽略的
`artifacts/windows-spb-audit-20261023/`，不提交厂商二进制/完整反编译。
其中 `analysis.log`、`decompiled/SpbThpTool.sys.c`、`instructions.txt` 对应本次证据。
第二次仅导出指令的 headless 运行出现 generated activator 重名告警，但退出码为 0，
输出包含全部十个指定函数及完整 literal 表；不能把该告警当成反编译器验证通过。

WDF 间接调用依据微软公开的
[KMDF function table](https://github.com/microsoft/Windows-Driver-Frameworks/blob/main/src/publicinc/wdf/kmdf/1.27/wdffuncenum.h)
复核。例如表偏移 `0x980/0x988` 是资源计数/描述符，`0x468` 是
`WdfInterruptCreate`，`0x5d8` 是 `WdfIoTargetFormatRequestForIoctl`，`0x9e0/0x9e8`
是 spinlock acquire/release。该参考用于识别这些表槽，不证明本机绑定了 KMDF 1.27。

## 资源、通道与物理连线

`0x1400024c0`（由设备 PnP 配置注册为 prepare-hardware callback）遍历 translated
resource list：

- 记录 serial connection 的连接 ID 到 context `+0x10/+0x14`；
- 记录 GPIO connection 到 `+0x18/+0x1c`；
- 最多记录两个 interrupt resource，分别安装 `0x140004770` 和 `0x140004600`
  回调，实际是否创建还受资源和配置条件控制；
- 由是否发现所检查的 GPIO connection 决定 master/slave 名字、`+0xf0` 通道标志
  和 `+0xf4` 固定传输长度，而不是从用户态打开的字符串反推硬件。

在 `0x140002d14–0x140002da4` 的指令中可直接核对：

| 路径 | 符号链接 | `context+0xf0` | `context+0xf4` |
|---|---|---:|---:|
| 该 GPIO connection 存在 | `\DosDevices\SPBTESTTOOL_MASTER` | 1 | `0x13c7` = 5063 |
| 不存在 | `\DosDevices\SPBTESTTOOL_SLAVE` | 0 | `0x153` = 339 |

`0x1400051a8` 将 serial connection ID 格式化为
`\Device\RESOURCE_HUB\%016x`，通过 `WdfIoTargetOpen` 打开总线 target。
`0x140006fa8` 使用 GPIO connection ID 打开另一 Resource Hub target 来执行 reset。
这两者是不同类型的资源，不应将 GPIO connection 直接当成 SPI CS。

**尚未取得 Windows 实机两个 devnode 的 `_CRS`/translated resources。**
INF 中同一行出现两个 ID，不足以证明 Windows 枚举了两个实例；也没有从该 `.sys`
读出 SPI 控制器物理地址或 chip select。Linux DT 当前只有 `spi0.0`，不能据此
声称 slave 不存在，也不能凭两个用户态 handle 直接增加 `spi0.1`。

## 事件栈生产、队列与用户态消费

```text
IRQ callback 0x140004770 ─┐
                         ├─ busy 门控 → 0x140004908 / 0x140006268
IntOpen 0x4001c00 ───────┘                    │
                                  SPB full-duplex 0x41814
                                            │
                           completion 0x140007d60
                                            │
                             最多两帧的软件 FIFO
                                            │
                          GetFrame 0x4001c28 / 0x140006970
                                            │
                              HalDevice → Windows solver
```

### 生产者

`0x140004770` 在 block 配置非零且 context `+0xd0`（in-flight 标志）为零时，
先置标志，再调用 `0x140004908`：

- master TX 前缀 `F3 30 00`，slave 为 `F5 30 00`；
- TX/RX 长度均来自 `context+0xf4`；
- `0x140006268` 构造传输描述并格式化 `0x41814` 请求；
- `0x140007b70` 安装 completion `0x140007d60` 后提交；
- completion 将 RX 数据复制到 FIFO 节点，最后经 `0x140008150` 清 in-flight 状态。

`0x140007f24` 起明确比较队列计数和 2：不足两帧才分配节点、复制整帧并接到尾部；
已满则不添加新节点，不是“覆盖最老帧”。`0x140006970` 从头取出节点，计数减一，
复制后释放。不能用“Windows 只保留最新帧”解释这份二进制的行为。

注意：队列完成路径中不能仅因有 RX 数据就断言传输成功。其状态检查、失败时是否
可能仍入队、短传输与计数器并发等细节尚未完整验证；**不应机械照搬这些实现风险**。
Linux 继续保留完整传输成功后才发布数据、失败保留上次完整证据的约束。

### GetFrame 与 timeout

`0x4001c28` 的 handler `0x140006970` 消费上述 FIFO；队列为空且 block 开启时
等待 `context+0xb8` 的 event。它不是一次新的 `F3`/`F5` SPI 命令。

`0x4001c2c` 的 handler `0x140006ce8` 将传入整数乘以 `-10000` 后存为相对等待
时间，随后用于 `KeWaitForSingleObject`。结合固定上游传入 200，此处是 **200 ms**，
不是 200 µs 或 3.2 ms。用户态累计的 3200 ms 恢复预算是另一个概念。
旧逆向文档中相应的微秒推断不能继续用于 Linux timeout 设计。

## IntOpen / IntClose 的真实含义

IOCTL literal 表 `0x1400045b8–0x1400045f0` 与控制流交叉复核：

| IOCTL | 本版本行为 | 关键证据 |
|---|---|---|
| `0x4001c00` IntOpen | in-flight=0 时置位并发出一笔整帧读取；已忙则直接完成请求 | `0x140004294–0x1400043d0` |
| `0x4001c04` IntClose | 跳到公共成功完成路径；该分支没有清队列/停 IRQ/复位 | literal `0x1400045c0`，分支 `0x140004200–0x140004208`，目标 `0x1400044a4` |
| `0x4001c28` GetFrame | 取 FIFO，必要时等待 | literal `0x1400045dc` → `0x140006970` |
| `0x4001c2c` SetTimeout | 更新相对等待时间 | literal `0x1400045e0` → `0x140006ce8` |
| `0x4001c30` SetBlock | 写入 block 配置 | literal `0x1400045e4` → `0x140006e50` |
| `0x4001c34` SetReset | 使用 GPIO Resource Hub target | literal `0x1400045e8` → `0x140006fa8` |

这是 **这个哈希版本的静态行为**。不能推广成所有 Windows 驱动都如此，也不能据此
移除 Linux 的 STREAMOFF/PM/shutdown 同步停机逻辑。尤其 Windows IntOpen 的
transport success 只表示请求已提交/分发，不保证后续有效帧或固件恢复成功。

## Linux 对齐情况与下一步

| 范围 | 当前 Linux | 后续动作 |
|---|---|---|
| master 读取大小 | `himax_read_raw()` 一次 5063 wire bytes，保留 5060 payload bytes | 长度已对齐；现有生产函数 host 测试继续保留 |
| 失败证据 | SPI 失败或 short transfer 不发布破损帧 | 保留，不照搬潜在失败入队行为 |
| 生产者 | threaded IRQ 持 `op_lock` 同步读 SPI | 可保留 Linux 原生模型；不能称为 Windows KMDF 异步 request 的逐项复制 |
| 消费者落后 | vb2 2–8 个 buffer；无 buffer 仍读硬件但丢弃本次交付 | 与 Windows 两帧满时丢新帧不同；先测真实延迟/队列积压，不直接改成每帧分配内存 |
| 启动清帧 | 在 IRQ 禁用时等待并直接读硬件三次 | Windows 消费已经被 IRQ 生产者填充的通道 FIFO；“同为三次”不等价 |
| 停帧恢复 | `StreamState::TimedOut()` 四秒后退出 daemon，由 supervisor 重启并重新初始化 AFE | 缺少先做无 reset 的通道补读；需要独立实现与故障测试 |
| slave | 没启用 | 不为手指移植顺带启用笔；取得真实资源映射与手指依赖证据后再决定 |

初版据此提出的实现约束是 **master-only、无复位的 producer kick**，而不是热重载 SPI/GPI：

1. 在 kernel client 内与 IRQ 共用有界、完整帧发布路径；任何 kick 都必须受
   streaming/panel/PM/shutdown、当前 epoch/sequence 和 owner 门控。
2. 必须解决“请求发送前 IRQ 已经产生新帧”的竞态，避免一次过时 kick 额外读走新帧；
   不能只把现有 `HX_RAW_RESET` 改名。
3. 对 kick 的提交成功、SPI 失败、实际收到新帧分开计数；没有新帧不能记恢复成功。
   不主动 reset baseline/solver，不改 epoch；超出有界预算后才进入原有恢复。
4. 测试无 IRQ、IRQ/kick 并发、新帧抢先到达、SPI short/error、STREAMOFF、panel-off、
   suspend/shutdown、过期请求、错误 owner 和 backpressure。host stub 不代替真实 IRQ 验收。
5. 继续保持完整内核中的 GENI/GPI 修复。严禁通过重载父控制器来模拟 Windows
   `IntOpen` 或 Resource Hub target reopen。

本文初版只完成后端审计。后续已经实现上述 master-only kick 接口及用户态恢复预算：
`VIDIOC_HIMAX_REARM`、`himax_rearm()`、共享 `himax_capture_frame()` 和
`ProducerRecovery`，具体约束见 [Offload 故障与恢复说明](touch-offload.md)。
测试直接编译生产捕获/校验/IRQ 函数，并以真实 pthread mutex 排列“旧 IRQ 等待时
补读先完成”的顺序；Windows 重试 oracle 直接提取固定上游 `readChannel` lambda，
只替换 HAL、时钟和 sleep，比较正常、IDLE 和已有累计耗时的补读/耗尽时间点。
这些测试不模拟真实 IRQ 电平、DMA 或 PM 并发。新增接口尚未部署；当前运行模块、
服务和启动文件保持原版本。
仍需实机验证 suspend/resume、快速入屏、多指和长期稳定性；手工切频仍只有 transport
成功而没有固件完成证据。这些缺口没有被本文消除。

### 新实现的构建与验证记录

- Release 构建、50 项 CTest 通过；包含生产 rearm/IRQ 提取、旧模块回退、正常/IDLE
  预算、直接编译 Windows lambda 的时间点对照，以及提取刷新/上游源文件漂移门控。
- 最终源码的 ASan/UBSan Debug 构建和完整 50 项 CTest 也通过（107.67 秒）；
  该结果覆盖所有上述 host tests，不是对运行内核、真实 DMA/IRQ 的 sanitizer 验证。
- `make -C tests clean test` 全部通过；legacy baseline/tracking 源码未修改。
- kernel diff 的 checkpatch：0 errors、0 warnings；`git diff --check` 通过。
- 编译服务器在独立 external-module 目录构建普通模块，通过匹配内核
  `7.1.8-gaokun3-himax1` 的 Kbuild。未修改共享源码、原 kernel-out 配置或父控制器。
- 取回后核对编译端/本机三份变更的 kernel source/header SHA-256 相同；模块
  vermagic 为 `7.1.8-gaokun3-himax1 SMP preempt mod_unload aarch64`，build ID
  `f42b16a7318df983e5015083f794b5196f2ea550`。
- 本机候选产物（被 gitignore 忽略）：`artifacts/producer-rearm-himax1/`。
  模块 SHA-256 为 `42bd7ea789679ce910e6df0bdbc7e446dcf413b1cd6bc10f0264d2347ebb70e6`；
  daemon 为 `b284a9bcce9e197b4458ccbb169748c4e1e11ff6e5f91ef56bdef5155d6e3dd5`。
  replay 未改变，哈希仍为 `50928ddf507a858e829eb98a296e9859c92fe31589eb4146cafd7f6e953ca15b`。
- 未执行真实 IRQ 丢失/停帧故障注入、PM/IRQ 并发、睡眠或新版本手势测试；
  没有将“软件模拟缺 IRQ 后可补读”当作“实机已经恢复”。
- 没有安装此候选。验收时仍运行 build ID `89b9985b76556d2179e11e83e6e1b426f2864efe`
  的旧模块；服务 PID `720293`、active、NRestarts=0，未重载父 SPI/GPI。

### 随后的实机试载：IDLE 补读是全零，不能宣称恢复

上述“没有安装”是首轮构建状态。随后在修复版测试内核上临时试载相同普通模块，
只卸载/加载 Himax client；GPI/SPI 从未卸载，前后 build-ID note 比较一致。
手动实机测试新增 `himax-rearm-hardware-test`，不注册为 CTest。

第一轮要求补读本身通过 frame checksum/非零校验，**未通过**：
IDLE 静默约 253 ms 时 rearm SPI 返回成功，交付 sequence 29，但数据全零。
没有把它当有效数据；试验立即退出并恢复旧模块/服务。
证据：`deploy-logs/producer-rearm-trial-zl0Gw9DG/`。

这暴露了用户态计时语义的缺口：新 HXST 元数据能结束本次等待，但无效内容不能清除
累计 rearm 次数。已修正 `ProducerRecovery::Frame(valid)`，只在有效 master 后
清次数；无效帧也不允许沿用旧 IDLE 标志。新增“SPI 成功但七次无效帧”的预算回归，
并检查 daemon 在真实帧校验之后调用该反馈。

第二轮显式使用 `--transport-only`，允许记录这一个全零补读，但不放宽默认有效帧
要求。相同现象再次出现：epoch 1、sequence 28 后补读得到全零 sequence 29，
随后没有再发送 kick，正常 IRQ 取得有效 sequence 30，仍为 epoch 1。
过时 token、非法大小/版本/reserved、其他 owner、立刻重复补读、STREAMOFF 的
拒绝结果均符合接口规定。30 帧 raw 记录含两帧无效数据（首帧及补读帧），
raw replay 正确拒绝非首帧全零，**不是回放通过**。

随后新 daemon 六秒正常采集 672 帧，bad=0、stale=0、无触摸报告，
strict replay 为 `session_frames=672 verified_frames=672 live_output_verified=1`。
试验结束已恢复原模块/服务，磁盘及启动文件未改；taint=4096、无新增内核 fault。
证据：`deploy-logs/producer-rearm-transport-stHtYCsR/`。

计时修正之后 Release 50/50、完整 ASan/UBSan 50/50（107.75 秒）通过；新增硬件
测试程序能正常编译，但从不由 CTest 自动访问硬件。新版 daemon SHA-256：
`16eeda6c95b104c0af49acd5b405d0c7871e9ef5e52e7b1e7448c24c2feec9ee`；
kernel candidate 未改。这里仍没有真实丢 IRQ、故障修复或触控质量证据。

### 随后正式安装到当前测试内核

已将试载的普通模块和 `b317a36` daemon 安装并运行：

- kernel/module：`7.1.8-gaokun3-himax1`，
  build ID `f42b16a7318df983e5015083f794b5196f2ea550`，
  SHA-256 `42bd7ea789679ce910e6df0bdbc7e446dcf413b1cd6bc10f0264d2347ebb70e6`；
- daemon：SHA-256 `16eeda6c95b104c0af49acd5b405d0c7871e9ef5e52e7b1e7448c24c2feec9ee`，
  PID `1080028`，`/proc/PID/exe` 与已安装文件逐字节相同；
- 三次统计 877→887→911 帧，bad=1（启动全零）不增长、stale=0，
  IDLE 帧 3→13→37，epoch=1 不变、NRestarts=0、无实际触摸报告；
- 没有触发 producer rearm；正常 IDLE 静默间隔被允许。不能用这一结果验证新恢复
  路径已在故障时成功，临时试载中全零数据的限制仍存在；
- SPI/GPI 运行 build-ID note 前后相同，没有重载；taint=4096，kernel health 无新增
  Oops/WARN/坏页。replay、systemd unit、Image、DTB、BLS entry 哈希/比较不变；
- `/boot` 和 ESP 测试启动项的 initramfs 均更新为
  `6d883d39af15864cd47a4fc5f67ea5c3147d4baf3e92e0fea671917b794917de`，
  解包 Himax/GPI/SPI/uinput 与已安装模块逐字节一致；
- 原模块、daemon 和两份旧 initramfs 保存在
  `deploy-logs/producer-rearm-install-bF7aAyWj/previous-*`，可回退。

未自动重启/休眠，没有改 Android 分区或默认启动选择；新 initramfs 尚未冷启动验收。
部署脚本、kernel health、服务统计和镜像核验保存在上述忽略目录。

## 后续：初始化前的 master bus-ready 检查

固定上游 `HimaxChip.cpp` 的 `ProbeBus/check_bus` 和 `DeviceRuntime.cpp`
`StepBringUp` 还包含一层前置条件：先读总线 `0x13`，如果传输成功但返回零，
只等待 `kBusPollInterval=500 ms` 再探测，不进入 `Chip::Init`，也不累计初始化
失败。此前 Linux 即使面板 follower 已启用也直接进入 reset/OTP/AFE，缺少这一步。

现已在生产 `himax_start_or_retry()` 接入 master-only 探测：

| 探测结果 | 动作 | epoch / reset / OTP |
|---|---|---|
| 当前电源/队列/PM/shutdown 门控关闭 | 不探测、不排重试 | 不变、无硬件访问 |
| SPI 成功、字节为零 | 500 ms delayed work，保留已有失败次数 | 不变、不初始化 |
| SPI 返回错误 | 累计一次失败，使用现有有界退避 | 不变、不初始化 |
| SPI 成功、字节非零 | 进入已有完整初始化 | 由原初始化管理 |

该规则匹配上游的非零检查，包括允许 `0xff`；不是新增强制 `0x31` 常量门槛。
不能将非零视作可靠供电/IC ready 的独立证明。Linux 只处理 master，Windows
双通道同时非零及每五次失败重开 HAL handle 的完整语义仍未逐项移植；
不能为模仿 reopen 而重新绑定父控制器。

`AfeStartReference.cmake` 现在还直接提取固定上游 `BusProbe`、`ProbeBus/check_bus`
以及 bus poll 常量。host 测试编译生产 startup/retry 本体，比较全部 256 种 master
字节、SPI 失败退避、持续零响应后转为 ready、全部 64 种电源门控，以及诊断
pre-SPI 注入仍在探测前。三个引用源文件完整 SHA-256 漂移时拒绝配置。

Release 50/50、完整 ASan/UBSan 50/50（107.74 秒）通过；kernel checkpatch
0 errors/0 warnings。普通模块在编译服务器的独立 M 目录构建，匹配
`7.1.8-gaokun3-himax1`。本机源码与服务器 `himax-v4l2.c` 哈希一致。
候选模块 SHA-256 `0eb279a609f68f9dd9dc3b3db6341a2df2f480f37db8b83410a041cf94411952`，
build ID `5b6f1d1cdba49f6729ffa7c3900ba74c92f65cd5`，
本机产物 `artifacts/bus-ready-himax1/himax-spi.ko`。
此处仍为未试载/安装状态，不能据 host 的零值注入声称观察到实机未供电或供电恢复。

### bus-ready 试载/部署与新发现：自动空补读触发 reset

`7f6c534` 普通模块随后临时试载：startup 及两次显式 reset 通过，
transport test 的 epoch 为 1→2→3，387 ms 时跨 epoch reset 被限频拒绝；
owner、参数、旧 epoch、STREAMOFF 检查通过。六秒 fresh 采集为 672 帧，
bad/stale=0，strict replay `live_output_verified=1`。该试验没有触摸报告。
结束恢复了之前模块/daemon，SPI/GPI build-ID notes 一致，taint=4096，无新增 fault。
证据：`deploy-logs/bus-ready-trial-coge3vFI/`。

随后将该普通模块安装到测试内核，daemon 起初仍为 `b317a36`。
启动统计 876 帧、bad=1（启动全零）、stale=0；然而继续运行期间出现以下记录：

| 内核启动后时间（秒） | 记录 |
|---:|---|
| 13359.850231 | rearm 传输成功，after_sequence=1425 |
| 13360.055748 | rearm 传输成功，after_sequence=1426 |
| 13360.260170 | rearm 传输成功，after_sequence=1427 |
| 13360.464542 | rearm 传输成功，after_sequence=1428 |
| 13360.670433 | rearm 传输成功，after_sequence=1429 |
| 13360.670636 | raw master health 请求恢复，原因 all-zero |
| 13361.088831 | AFE epoch=2 |

之后统计 2311 帧、bad=6（含启动全零）、stale=0、106 次触摸报告；
`producer_rearm requested=5 transferred=5 valid_after=0`，
一次 AFE reset。daemon PID 不变、systemd 无自动重启、内核没有新 Oops/WARN/坏页。
因此“服务没重启”不能代表没有触控恢复/复位。

没有该时段的同步 raw/完整逐帧状态，不能直接断言是主动误读、进入 IDLE 后状态滞后、
真实 IRQ 停顿或其他硬件故障。只能确认现有主动补读路径制造/观察到五个空包，并
将其送入正常 IRQ 的混合错误阈值，随后触发 reset。此前主动 IDLE 试验已经证明
空补读不一定表示 producer 坏了，所以 **当前自动恢复策略的实机适用性未获验收**。
源码的 50 项测试也没有覆盖这个真实时序，不应以其全绿反驳现场。

已将已安装 daemon 临时回退到原 `c00844e`：
SHA-256 `72ce2c8d8ef5e515769622172d35263f24ffd060b9e23e7334fbbac3e1a1847d`，
四秒等待策略，不调用 rearm。保留 `7f6c534` 的 kernel module（bus-ready 与该问题独立），
build ID `5b6f1d1cdba49f6729ffa7c3900ba74c92f65cd5`。
回退后 PID `1256329`、epoch=4，统计 1534 帧、bad/stale=0、109 次触摸报告、
121 个 IDLE 帧、NRestarts=0；只证明这段运行，没有证明根因已消除。

该测试启动项的 `/boot`/ESP initramfs 已同步：
`471ad28949f94598a720930cc15a79720020adcaf3195675187f349f18d443b2`，
解包 Himax/GPI/SPI/uinput 与已安装文件一致。Image、DTB、BLS entry、
SPI/GPI 运行 notes 不变，未自动重启；新镜像未冷启动验收。
旧模块、两份旧 initramfs、部署/回退日志在
`deploy-logs/bus-ready-install-8HLvWGMV/`；
`service-stall-timed.log` 保留上述现场，`service-after-daemon-rollback.log` 为回退后统计。

重新部署新 daemon 前必须补齐：

1. 为主动读取的空包保留可核实来源；不能直接等同正常 IRQ 的错误帧。
2. 分清元数据包交付和有效固件/自然 IRQ 进展，防止主动空读刷新超时证据。
3. 检查 active→IDLE 与无手指静默的等待窗口，尤其缺少 Windows slave 通道时的差异；
   没有实机状态前不把一个新的时间常量称为根因修复。
4. 为上述分支补上 host regression、会话/strict replay 来源记录和实际过渡采集。

为避免未来默认部署重新打开未验收的路径，HEAD 新增 `--experimental-rearm`，
默认 false；默认仍四秒等待后由 supervisor 重建流，`--info` 和 `--observe` 禁止
组合实验开关。生产 `ProducerRecovery::Next` 有明确 disabled 分支；host regression
验证关闭时不会在 200/3600/4000 ms 申请 kick、超过四秒才耗尽，显式启用才进入新策略。
CLI 测试检查组合拒绝及 help 默认提示；引用集成检查 daemon 将实际选项传入 policy。
内核窄接口保留，没有用临时开关宣称移植完成；已安装 daemon 仍是回退版本。
带此默认门控的源码 Release 50/50、完整 ASan/UBSan 50/50（108.38 秒）通过。
本机新构建 daemon SHA-256 为
`0d8344638fc78d523825cd9f1b8715de30d98d039ca2798b431a786f7003e6ba`，
未替换上述已回退的运行程序；最终核对 service active、PID `1256329`、
NRestarts=0，运行 exe 与旧版本备份一致，两份新测试 initramfs 相同，kernel health
没有新增 fault。这些结果没有解除自动 rearm 的现场验收缺口。

## 后续：主动读取来源、空轮询语义与回放

为解除“主动空读和 IRQ 错误混在一起”这一具体缺口，新代码加入：

- `HX_RAW_FRAME_REARM`，只在 `VIDIOC_HIMAX_REARM_TAGGED`（private +3）请求的
  完整数据包上设置。普通 IRQ 无此标志，同一设备后续 IRQ 也不继承它。
  原 +2 ioctl 保留旧 flags，避免旧调用者收到未知字段。结构尺寸与 ABI v2 不变。
- 新 host 请求 +3；旧模块返回 ENOTTY 后保留四秒等待。禁止退而使用 +2 再通过
  猜测时间或 sequence 来给未知来源包分类。
- 只将“明确来源为主动读且 checksum body 全零”当作 no-data。它不调用求解器，
  不修改 baseline、不清真实 IRQ 错误、不产生第五帧 IRQ 错误恢复，也不取消已存在的
  reset。有效主动读仍解算，checksum/filler/size 错误仍真实累计/恢复。
- 空轮询虽交付了 HXST 元数据，不代表固件自然 producer 有进展。
  `ProducerRecovery::EmptyRearm()` 只消费 delivery stamp，不刷新等待起点、
  attempt count 或最近一次完成后的 grace。mode 保留上一有效帧判断；普通无效 IRQ
  仍使上一 mode 不再被信任。
- grace 从 ioctl 返回开始计算，而非从发起请求计算。host 测试模拟 150 ms 的
  SPI 阻塞，确认返回后仍有完整 20 ms 等待，重试计时也从完成时刻继续。
- `RecordedFrame`、HXST reader、SessionReader 都保留来源；strict replay 使用同一个
  `MasterFrameHealth` 规则。fresh HXSE 1 侧车语法不变，旧 reader 见到未知 raw flag
  会拒绝；新 reader 对 v1 帧伪造该 v2 来源也拒绝。
  raw replay 将 `empty_rearms` 单列为 no-data，不算 valid，纯空文件仍不通过。

测试构造八个连续 tagged 空轮询，再加入真实五帧 IRQ checksum 错误和 bus filler：
只有真实故障触发两次 reset。将 raw 文件里八个来源标志删除后，strict replay
必须因为缺少对应的 reset action 而失败；恢复原来源后重新通过。这验证了落盘/
配对/回放链路不能静默丢失 provenance，而不仅是单测里给同一函数传一个 bool。
另外覆盖 legacy +2 flags、同设备后续 IRQ、不支持模块回退、空轮询 deadline、
错误计数不清除和 tagged 非空错误。

最终 Release 50/50、完整 ASan/UBSan 50/50（107.37 秒）通过，kernel diff
checkpatch 0 errors/0 warnings。服务器已构建匹配 `7.1.8-gaokun3-himax1`
的普通模块，源码/header 哈希与本机一致：

- module SHA-256：`cbef3bb520d0824df76d349ca940c1f8b00fda8df7055e3161245dba32afb335`；
- module build ID：`5faf3ab0c3fa1d7c979793919307a738766971f2`；
- daemon SHA-256：`2f3ed524ae6ac8694749e94d931ad0bce2f1569c0ac00eead5d855aa145423b9`；
- replay SHA-256：`bc2f72d9248c11dab8dffa412583549490b04ab21c4e455eed36e2d6dc756642`。

当前为候选构建，尚未试载/安装。自动恢复仍须显式 `--experimental-rearm`；
active→IDLE 过渡、缺少 slave 通道的等待语义、真实 lost IRQ 和手势质量尚未验收。
不能因为 tagged 空轮询不再触发五帧 reset，就声称有界 producer 超时也不会误判。

### tagged-origin 实机试载与保守默认部署

候选随后受控试载，未启用自动补读；只由专用 hardware test 在 IDLE 静默约 255 ms
时主动读取一次。证据：
`deploy-logs/tagged-rearm-trial-aUkkPVi9/`。

- epoch=1、sequence 29 为全零补读，flags=6（CHARGER=2、REARM=4）；
- 不再主动读，正常 IRQ 的 sequence 30 有效，flags=2，仍为 epoch=1；
  这验证同设备后续 IRQ 不继承主动读来源，不能证明丢中断恢复；
- 30 帧 raw 中有一个启动无效帧和一个 tagged 空轮询；新 raw replay 为
  `rejected=1 startup_allowed=1 empty_rearms=1`，保留 no-data，未将其称为有效数据；
- 同一新 replay 读取旧的 untagged IDLE 试验记录时，仍报两个 all-zero、
  `empty_rearms=0` 并返回 1，没有给历史未知来源包“补造”标记；
- 默认关闭自动补读的新 daemon 另录制 672 帧，bad/stale=0、无触摸报告，
  strict replay `session_frames=672 verified_frames=672 live_output_verified=1`；
- 权限/版本/reserved/epoch/token/限频/STREAMOFF 检查通过；SPI/GPI notes 相同，
  无新 kernel fault、taint=4096。临时试载结束先恢复旧模块/daemon，磁盘/boot 未改。

完成试载后，已正式安装 `1674f8e` 普通模块、daemon、replay，保守默认不变。
运行 PID `1445301`，`/proc/PID/exe` 与已安装 daemon 一致；模块与上述候选哈希一致。
日志明确为 `producer_rearm=disabled`；统计 878→917 帧、bad=1（启动全零）不增长、
stale=0、IDLE 帧 5→44、epoch=1、NRestarts=0、没有触摸报告或新 reset。

`/boot` 与 ESP 同一测试启动项 initramfs SHA-256：
`19fab49c4722da84a7d8d8387e06d15b7c09ee9c7ea66961845ef9cb5998a580`；
解包 Himax/GPI/SPI/uinput 与已安装模块逐字节一致。Image、DTB、BLS entry、
SPI/GPI 运行 notes 和 systemd unit 不变；没有自动重启或改 Android 分区。
旧模块、daemon、replay、两份旧 initramfs 与核验日志在
`deploy-logs/tagged-rearm-install-jZr7HanI/previous-*` 及同目录，可回退。

此轮解除了来源缺失和空包错误计数的具体问题；自动策略仍需过渡/真实故障验收，
更新镜像仍待冷启动。默认不产生 kick，不能用默认运行统计代替实验模式验收。

## EnterIdle 命令与读等待许可

继续核对固定上游的 `HimaxAfe.cpp::AfeController::EnterIdle()`：
`send_command(master, 0x0a, ...)` 成功后立即调用 `ArmIdleRetry()`，并未等待下一帧
报告 IDLE。`HimaxChipFrame.cpp` 随后依据合法 master 的 idle word 更新这个许可：
1 置位、0 清除、其他值保留；这与 `afe_mode` 的 1=Idle、其余=Normal 不完全相同。
Linux 原先用 `lastFrameValid && LastAfeStatus().kIdle` 决定等待长度，因此漏掉了命令
成功但确认帧尚未到达的窗口，也漏掉了非 0/1 的保留语义。

当前用户态新增 `FrameReadMode`：

- 命令传输成功且为 EnterIdle 才设置读等待许可；失败保留先前许可，其他命令不改；
- 只在 `ToThpMaster()` 完整验证成功后读取帧尾两个字节，合法 active 立即清除；
- 普通无效帧和实际非空补读错误清除许可，不读取转换失败后保留下来的旧 THP；
- 带来源的全零补读是 Linux 明确的 no-data 例外，保留许可；
- AFE 停止、新 epoch 或重新开始时清除。没有读取 slave，也没有启用手写笔；
- 恢复预算只使用该独立许可，不改 solver 的 AFE status、baseline 或 IRQ 数据路径。
  默认仍关闭自动补读；统计增加 `read_idle_retry`，不能把它当成固件完成确认。

验证使用实际 Windows `EnterIdle()` 分发及原样提取的 suffix 更新语句，源码 SHA-256
门控保持有效。覆盖五槽及每步事务失败、两种初始许可、全部 65536 个 suffix word、
转换失败留下旧 THP、空轮询、迟到确认和停止/新 epoch；其中模拟确认一直未到时
200 ms 不申请 kick，3600 ms 才进入原有有界恢复。合法 active 帧到来后恢复
200 ms 等待。Release 50/50 与完整 ASan/UBSan 50/50（107.29 秒）通过。

先前正常实机记录 `deploy-logs/session-WepbEdEp/` 为 878 帧、epoch=3，
无手指帧、bad/stale=0。sequence 871 下发 EnterIdle 成功，但本帧尚未报告 IDLE；
sequence 873 首次报告 IDLE，相对命令行的 `hostTimeNs` 约 13.4 ms。
该时间戳在命令执行前，不是精确的命令完成时间。之后五个 IDLE IRQ 间隔约
3050.008、3015.912、3016.701、3017.345、3016.670 ms。

新 helper/policy 对这份记录做离线时序检查，得到一次“命令先于确认”的许可设置、
六次 IDLE 等待、最大约 3050 ms、没有预测 rearm/exhaustion；新 replay 严格验证
878/878 帧输出一致。工具源码/结果保留在
`deploy-logs/read-mode-shadow-hY6AbfwS/`，检查的是记录的交付时刻，不模拟每次
50 ms live poll、调度及 SPI 完成时刻。正常确认约 13.4 ms 的这份记录不解释此前
连续空读现场，也不证明实机丢 IRQ 已恢复。

### 本机试验与用户态部署

源码提交 `039cdf0` 随后在同一测试内核完成 25 秒临时实验会话，显式传入
`--experimental-rearm`，没有禁用 IRQ、注入故障或卸载模块。epoch=7，878 帧全部
有效，bad/stale=0，IDLE 6 帧，EnterIdle 一次传输成功，`read_idle_retry=1`；
没有 rearm、empty poll、reset 或 producer stall，录制 dropped=0。
strict replay 为 878/878；同一离线时序工具看到一次命令先于确认，最大 IDLE
交付间隔约 3050.11 ms，没有预测补读或预算耗尽。
这验证实验路径在本次正常 IDLE 下没有误触发，不是主动恢复成功验收。

随后只更新 `/usr/local/bin/himax-touchd`，SHA-256：
`9bfc6684f823ad35dd65596e629ad807c795a9c94171892bd0b968fd2d18721e`。
常驻服务恢复无实验参数的默认配置，PID=1602714、epoch=9、NRestarts=0；
877 帧时 bad/stale=0，IDLE 5 帧，读等待许可为 1，没有 reset/stall。
两轮都没有实际触点报告。内核/触摸模块/控制器没有重新加载，Image、DTB、
两份 initramfs、BLS entry、systemd unit、replay 的哈希及三个模块运行 notes
前后相同；kernel health 无新增故障。旧 daemon、实验 raw/侧车、部署脚本与日志在
`deploy-logs/read-mode-install-2OwoOVHt/`。自动补读仍默认关闭，没有自动重启。
