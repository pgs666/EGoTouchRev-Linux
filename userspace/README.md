# Linux 用户态触控求解

当前本机部署（2026-10-24）：测试内核 `7.1.8-gaokun3-himax1`，已安装支持
精确 token 异步重建的普通模块、配套 daemon/replay 和 `himaxctl`，保留异步启动清帧、
两帧队列及边缘首点修正。**常驻服务已开启先无复位补读、耗尽后同进程 AFE 重建。**
active/IDLE 的等待预算分别按 master-only Windows 适配策略执行；失败/超预算再
退出给 supervisor。裸运行 daemon 不加选项仍保留四秒等待回退；service 显式添加
`--experimental-rearm --experimental-restart`。
可选运行中窗口录制仍默认关闭，不改变 baseline/追踪算法。下文增量记录按时间
排列，早先“尚未安装/默认关闭补读或重建”是当时状态；当前部署以本节及末节为准。

运行中的手动控制：

```sh
sudo himaxctl status
sudo himaxctl calibrate
sudo himaxctl reset
sudo himaxctl calibrate --wait
sudo himaxctl reset --wait
```

`status` 查询产帧状态、epoch/sequence、错误与当前触点数量。`calibrate` 发送 AFE
校准请求，返回成功只表示传输完成；固件完成仍以随后 calibration-done 为准。
`reset` 调度客户端 AFE 重新初始化，不卸载模块、不退出 daemon；随后用 `status`
确认 epoch 增加、running=1 且 sequence 继续增长。它不承诺消除所有触控异常。
服务默认使用 `--control`，由 systemd 创建 root 专用的 `/run/himax-touchd`。
操作复用 daemon 的 V4L2 队列所有权，在求解线程串行执行，关屏或自动重建中会
拒绝硬件命令。操作日志可用 `journalctl -u himax-touchd` 查看。
`--wait` 在客户端等待：校准以同一 epoch 后续有效帧中的新完成标记为准，reset
以新 epoch 的实际有效数据为准；不阻塞求解线程。`status` 的 `calibration` 是
最近一次手动校准状态：none/pending/done/timeout/interrupted/failed；未观察到
完成标记满八秒为 timeout，AFE 停止或重建会中断尚未完成的请求。
`valid_epoch/valid_sequence` 与普通 epoch/sequence 分开，避免把初始化完成通知
或无效包误认成 reset 已恢复数据。IDLE 下校准反馈可能要等下一个慢速帧周期。

读帧恢复使用 `VIDIOC_HIMAX_REARM_TAGGED`，普通运行约 200 ms、已允许 IDLE
长等待时约 3600 ms 没有进展后申请一次无复位补读；重试预算耗尽后才申请 AFE
重建。来源标记区分主动空轮询与真实 IRQ 异常，只把“标记补读 + 全零”当 no-data，
不清已有错误或更新 baseline；checksum、bus filler、长度错误继续按异常处理。
EnterIdle 传输成功立即允许长等待，合法 active 帧才取消，避免过渡期过早补读。
旧模块不支持 tagged ioctl 时保留四秒回退，观察模式不主动恢复。
`himaxctl status` 暴露 `producer_rearm`、补读请求/空读/之后有效数据，以及自动
AFE 重建计数。传输成功和后续有效数据分开，不把后者当作补读修复故障的因果证明。
严格会话回放保留来源并验证输出，不复现真实硬件操作。历史空补读导致 reset、
回退、修正及受控试载记录见 [SPB 后端审计](../docs/windows-spb-backend-audit.md)；
真实故障、手势/PM 和长期运行验收仍有缺口。

当前模块另通过生命周期事件尾部传递停止原因：明确关屏、休眠、停流或关机时，
取消待完成的恢复等待并重置连续故障预算，保留旧 epoch 隔离；重建中的临时停止
不取消超时。`status` 的 `stop_flags` 和 `afe_interrupted` 可用于观察，旧模块缺少
原因时沿用原行为。常规帧路径不增加 ioctl 或确认帧。

默认数据路径为：

```text
HX83121A SPI → V4L2 META_CAPTURE / HXST → himax-touchd → uinput → libinput
                  内核传输层              原版 TSACore
```

内核只负责 SPI/AHB、复位、固件启动、屏幕电源生命周期、充电状态和有界采集队列。
baseline、AFE 状态解析、噪声处理、检测、跟踪和滤波由用户态完成。内核不再注册
触摸 input device；`himax-touchd` 是唯一手指输入生产者。

uinput 使用 HXST 的单调时钟采帧时间，同一帧各事件共享时间戳，避免把用户态
调度间隔当作采样间隔。手动/超时释放使用当前时间，后续样本不会倒退输入时间；
`BTN_TOUCH` 位于每帧最前面。此适配不增加确认帧，也不改变上游算法或坐标。

算法直接编译上游 EGoTouchRev v1.4.0 的全部实际启用模块、`HimaxAfeAdapter`、
`TsaCoreSolver` 和参数表，submodule 固定在
`4d4f53d0bd8563d20850d5f380bcd6a8704c1e81`。不构建 Windows 服务、VHF 或 GUI。
主动笔硬件采集未启用，算法的笔状态保持无笔默认值。
文中“原版”指这个固定版本的上游 C++ 重写实现，不是直接加载华为 Windows DLL，
也不是 Himax 发布的算法源码；移植成功不等于已证明与零售版驱动逐帧等价。

构建时生成五份上游文件的本地适配/修复副本，详见
[`cmake/PrepareUpstream.cmake`](cmake/PrepareUpstream.cmake)：生成上游
`TsaCoreSolver.cpp` / `Sys.cpp` 的副本，在首次初始化、每帧和复位内部应用宿主方向；
`Sys.cpp` 同时清除本地边缘信号历史；生成 `Tsa.cpp` 副本，在峰检测前观察已处理的
Dif 边界信号；
生成 `TouchReport.cpp` 副本，在首次获准上报时调用本地边缘起点修正；
生成 `Contour.cpp` 副本，限制插值取窗的行列，修复最后一行/列的越界读取。
submodule 文件保持不变，原始文件的完整 SHA-256 不匹配时停止配置，要求重新审查适配。
生成文件位于 `build/upstream-overlay/`；其余上游算法源码、参数表和布局没有改动。

测试构建另从固定版本 `HimaxProtocol.cpp` 提取原始 AFE 构包函数，完整源文件哈希不符
就拒绝配置；它仅链接到 `himax-afe-test`，不是运行时算法适配。内核生产邮箱协议
在 `hx-afe.c`，测试比较全部命令/参数组合，并检查三步寄存器事务的顺序、失败和重试。

内核的 `himax_mcu_power_on_init()` 调用共享的
[`hx-raw-init.c`](../touchscreen-hx83121a-dkms/hx-raw-init.c)。默认 V4L2 路径执行
`hx_raw_start_scan()`，其中 `hx_raw_prepare_scan()` 对应 Windows `Chip::Init()`
实际调用的前置事务：

```text
flash reload enable=0
→ SRAM inspection unlock=0（最多 20 次）
→ sorting=0（最多 10 次）
→ N-frame=1
→ N-frame=0x7f0c0001
```

解锁、sorting 和两次 N-frame 均完整读写四字节，但只比较低两字节；
前两项对传输或比较失败都重试，耗尽返回 `EINVAL`，由外层从硬件 reset 重试。
N-frame 任一笔失败立即退出，不能继续启用 raw。这里没有延时或 sense-on。
后续又写 N-frame=1，不构成省略前置事务的依据。legacy 入口保持原有准备流程。

随后仍在 `hx_raw_start_scan()` 内执行 Windows 的 master 后半段：

```text
RawOut=0 → sorting=0（只写）→ N-frame=1 → reload 状态=0
→ 平台 sense-on/reset
→ 完整 reload word == 0x000072c0（最多 30 次）
→ RawOut=0xf6（写/读低 16 位验证，最多 50 次）
→ master SRAM password=0x5aa5（只写）
```

每次 reload 未就绪时依次读取四个固件状态寄存器，再等待 11 ms；状态辅助读取在
首个错误处结束，但不阻止下一次 reload 轮询。reload 本身的读取失败立即退出；
30 次均未就绪则返回 `ETIMEDOUT`，最后一次未就绪之后也等待 11 ms。没有另设
A8==5 门槛。RawOut 重试传输和比较失败；耗尽后保留最后一次错误，区别于前置准备
统一返回 `EINVAL`。最终握手由固件消费，不做相等读回。外层失败从完整硬件 reset
重试，日志给出失败阶段。

默认路径不再随后调用 legacy `himax_restore_raw_runtime()` 重写 AP notify、
sorting/N-frame、RawOut 和握手；Linux 充电状态仍由独立 `charger_work` 应用。
native sense-on 由共享 `hx_ahb_sense_on()` 执行：接口唤醒、ISR 清零后至少等待
11 ms，再执行 Linux 平台的 GPIO reset。
`HIMAX_V4L2=0` 仍走既有 `hx_raw_power_on()`：保留 sorting 读回、A8 检查和 legacy
runtime 重放。这条旧路径的故障测试继续保留。

已安装的前置检查：V4L2 bring-up 在上述完整初始化之前，
先以 `F3 13 00` 读取一个 master bus 字节。与固定上游 `DeviceRuntime::StepBringUp`
一致，传输成功但读到零时，只调度 500 ms 后再探测，不执行 reset/OTP/AFE，不递增
初始化失败次数或新建 epoch；读取失败走既有 200→500→1000→1500→2000 ms 退避。
所有 panel/PM/stream/shutdown 门控在探测前执行，诊断 pre-SPI 失败注入也在探测前。
这里只移植启用中的 master 通道，不访问 slave，不通过重载父 SPI/GPI 模拟 Windows
handle reopen。非零（包括 `0xff`）只满足 Windows 的弱前置条件，不代表 IC 身份、
固件已 ready 或 raw 质量；后续 identity/reload/raw 检查保持不变。

直接源码对照通过
[`RawPrepareReference.cmake`](cmake/RawPrepareReference.cmake)，提取未经修改的
`himax_mcu_reload_disable`、`himax_mcu_assign_sorting_mode`、`hx_set_N_frame`、
`himax_switch_mode_inspection`、`write_and_verify`、`himax_mcu_power_on_init`、
`himax_mcu_read_FW_status`、`himax_switch_data_type` 八个函数体，以及 `Init()` 从准备
到 master password 的实际调用片段；寄存器值也从上游 `HimaxRegisters.cpp` 提取。
五份输入文件（含实际使用的 `HimaxByteUtils.h`）都校验完整 SHA-256。引用实现只替换
HAL 读写、sense-on 和 Sleep 为共用 fake bus；对照每笔事务、延时及返回值，包括所有
正常事务故障、20/10/30/50 次边界、低位/全字比较、状态读取错误和最后一次错误优先级。
引用实现仅链接到测试，不进入模块或 daemon。

启动 reset 已接入 `hx_ahb_hw_reset()`：清 `0x100072c0` → RESET 低/高 → 开启 burst，
随后等待 10 ms，再执行 Linux 的只读 IC ID 检查；首次 OTP 后再执行同一 reset。
不再每次为读 IC ID 而走旧 `himax_sense_off(false)` 的 safe-mode 路径，OTP 自己的
安全模式序列仍保留。probe 必须获得 reset GPIO，并以 logical low/released 获取、
等待 50 ms；不能像旧入口一样先保持 reset asserted，使协议的第一笔清 reload
写入失效。GPIO 映射沿用本机 ACPI 极性，低/高两步分别保留原有 20/50 ms 等待。

完整启动仍有差异：Linux 保留只读 IC ID 检查；GPIO 实际时序未纳入 oracle。
Windows `SetReset` IOCTL 内部时序未知，不能用无延时脉冲替代。Linux 的充电集成、
清帧及 IRQ 接口也不同，slave/pen 按本项目范围保持关闭。不能据此声称完整
`Chip::Init()`、物理 SPI 生命周期或手动切频已验收。

### AHB 总线协议

默认 V4L2 的寄存器读写和 interface-on 已接入共享
[`hx-ahb.c`](../touchscreen-hx83121a-dkms/hx-ahb.c)，对应固定版本
`HimaxProtocol.cpp` 和 `HimaxChipLifecycle.cpp`：

- 每笔寄存器事务先写 `0x13=0x31`，再写 `0x0d=0x12`（长度 ≤4）或 `0x13`
  （长度 >4）。使用四字节小端地址；写回调的 length 仅指 payload，地址另计四字节；
- 读取经 `0x00` 选址、`0x0c=0` 选择读方向，再从 `0x08` 取数据。
  长度 >256 在任何 I/O 前返回 `EINVAL`；写入容量由 native SPI 回调在加地址长度前
  检查，超出返回 `EMSGSIZE`；
- 成功和失败均保持当笔事务选择的 burst 模式，不再额外反向切换；
  原始总线错误立即返回，不增加协议层重试或恢复事务；
- interface-on 先 dummy-read `0x08`，再配置/读回 burst，最多十次，每次不匹配后
  等 1 ms。按上游行为，十次传输成功但仍不匹配时继续启动；任一传输失败立即返回。
  这不证明固件就绪，仍必须完成后续 IC ID/reload/raw producer 验证。

旧 `HIMAX_V4L2=0` 保留 `0x10/0x11`、事务结束反向切换和严格 interface-on 失败策略。
OTP 专用序列原本就使用独立的 `0x12/0x13` 原语，本轮未改动。

[`AhbReference.cmake`](cmake/AhbReference.cmake) 在测试中直接编译未经修改的
`burst_enable`、`ConfigureBurstForLength`、`register_read`、`register_write`
和 `himax_mcu_interface_on`，以及 `hx_hw_reset_ahb_intf`、`hx_sense_on`；四份来源
文件（含寄存器常量与 ByteUtils）校验完整哈希。
共用 fake bus 对照命令、地址、全部 payload、返回值、调用后缓冲区、延时与停止位置。
覆盖四种地址、0～512 长度域、所有有效事务步骤的故障、十次 interface-on 边界、
native 容量错误和最大 u32 长度。源码提取集成测试验证原函数未改、哈希漂移/缺失拒绝
和无变化配置不改写产物。

reset/sense-on 对照逐笔验证 reload 清零、GPIO 两次切换、burst、ISR 和 11 ms 等待，
覆盖普通与延迟 interface-on 的所有总线/GPIO 故障停止点。sense-on(false) 为实际
生产路径；true 分支只验证不会切 GPIO、而是写 `0x31` 两字节零，并不在默认启动中
启用。GPIO 回调缺失在任何 I/O 前拒绝，native GPIO 缺失也不再被当作复位成功。

这个 oracle 的边界在 Windows `ReadBus/WriteBus/Sleep`：native SPI 封包、重试、
控制器实际传输长度、GPIO、Windows THP IOCTL 内部时序仍需单独验证。
不能由协议级对照或 raw 帧有效直接推断物理 SPI、完整生命周期、手动切频均已对齐。

### 原生 SPI 封包与短传输

**部署状态修正（2026-10-23）：当前严格长度检查版本尚未完成安全实机部署。**
先前单独替换 GENI 控制器的试验触发 GPI DMA 配置的释放后使用，已经出现 Oops；
不得将下文中的采集成功当作整体验证通过。运行中仍为原控制器与 `f27e70e` 触摸模块。
后续需要 [两项宿主修复](../kernel-patches/README.md) 一起集成到内核并重启验证，
不再热替换控制器。

默认 V4L2 路径的读取使用与 Windows `HalDevice::Ioctl()` 相同的最多十次尝试，
legacy 保留三次；成功但长度不足的读取在 OS 事务返回之后直接失败，不额外重试。
native 检查 `spi_message.actual_length` 必须等于整笔长度，完整读回后才复制给
调用方；拒绝短帧和不可能的超长返回。读写容量检查先验证 header/地址空间，
再做减法或加法，避免小容量下溢及最大 u32 长度绕过边界检查。

测试直接编译当前 `himax_spi_read`、`himax_spi_write`、`himax_write` 和 AHB 写适配，
与固定上游未经修改的 `IoctlOnce/Ioctl/ReadBus/WriteBus` 对照。比较所有 256 个命令、
header/可选四字节地址/payload、多个长度直到 native 容量边界、每个重试停止位置、
短读取和取消；源码生成器检查上游 cpp/h 哈希、原函数体完整性及 native 改动刷新。
Windows 的 `PerformSyncIo` 和 kernel `spi_sync` 被同一个同步假控制器替换；
这不验证 Windows overlapped 等待、THP IOCTL 或 Linux SPI 控制器的物理行为。

以下是有测试的 native 保护，不宣称逐项等同 Windows：

- 每次读取重试都重建 TX header 和 dummy 数据，防止共用 RX/TX 缓冲区在失败时
  被改写；独立测试九次部分损坏后第十次成功及输出与 scratch 重叠的情况；
- `ECANCELED` 对应 Windows 取消后停止重试；Linux 另对 `EINTR/ERESTARTSYS`、
  `ESHUTDOWN/ENODEV` 立即返回，保留原始 errno；
- Windows 当前 `WriteBus` 不验证返回长度；native 额外拒绝短写，但同样不重试写入，
  避免自动重复有副作用的命令。

重试次数增加只影响读取发生总线错误的路径，不增加正常触控的确认帧或等待。
真实短传输/取消/控制器移除故障注入尚未由上述 host 对照证明。

实机发现的前置条件：本机 GENI GSI 完成回调提前设置 message success，造成
`spi_sync=0 / actual_length=0`。严格检查会拒绝第一笔写入并停止启动。
对应宿主补丁及适用条件见 [SPI 控制器修复](../kernel-patches/README.md)；
不能单独升级触摸模块而忽略此依赖，也不能据此放行任意零长度传输。

### 边缘滑入的首报修正

上游 EdgeRecovery 按确认按下时的当前位置决定是否恢复边缘起点；快速滑动在
debounce 后可能已经越过窄修正窗口。上游外推还要求两个坐标分量均非零，因此严格
轴向滑入可能没有补回边缘历史。Linux 的旧内核算法另有 edge fast-start，新路径
没有默认编译它。不能把这个回归归因于 V4L2/uinput 额外排队。

本地 [`edge-onset.cpp`](edge-onset.cpp) 在正常防误触准入、历史处理和坐标滤波之后，
只修正第一次获准上报的坐标。使用最初候选位置与当前运动方向反向求边界交点，
支持一维位移为零；不改变 raw、baseline、down/up 状态、debounce、ID 或回放队列，
不设置上游会影响 lift-off 与上报准入的 history-replay 标志，也不再等待确认帧。
它是本地补偿，不是上游/华为原厂逐函数等价实现。

适用条件为：单指、正常信号/形状、无全局噪声/异常、尚未报过点，首次检测不超过
36 ms；运动需明显朝内且反向轨迹能到达该边。起点在已有 ER 最大范围以内时沿用
原有条件；超过该窄带时，必须同时满足下列边界证据：

- 首个候选出现的前一帧，有一处达到当前 `edgePeakThold` 的局部贴边信号，
  边缘值大于相邻内侧值；信号只能有一段、最多五格，其他边没有同时出现信号；
- 再前一帧的这条边安静，且这些帧都没有历史触点、噪声、异常或基线复位；
- 前置帧间隔均不超过 18 ms，边界信号的位置与反向轨迹交点相差不超过一格；
- 反向运动在候选创建前的一个采样间隔内能到达该边，且仍受原有最大位移限制。

固定八帧历史只存四边的摘要，不保存网格；仅在没有历史触点时扫描周长，复位时清空。
它观察 baseline/CMF/GridIIR 后、峰检测前的 Dif，不修改检测阈值或 baseline。
`noiseThold` 在该算法中不是统计噪声幅度，不能用它的倍数作为前置边缘信号门限；
样本中的贴边信号为 581，低于 `fingerThold=600`，但超过 `edgePeakThold=420`，
因此可以用于确认来源，同时仍不绕过后续正常触点准入。

角落、静止、沿边、向外、多指、弱峰/分裂区域、异常或长时间被抑制的触点不修正。
如果首次检测已越过窄带，又缺少上述前置边缘信号，则保留上游坐标；无法仅凭两帧区分
“屏外滑入”和“在很靠近边缘的位置按下后朝内移动”，后者也可能被当作边缘滑入。
后续实机应重点检查这项取舍，不应无限扩大范围。

复现/对照工具只在 `BUILD_TESTING` 下编译：

```sh
# rotation、原始面板边（左0/右1/上2/下3）、每帧移动的格数、切向漂移格数
userspace/build/himax-solver-edge-test 3 3 3 0
userspace/build/himax-solver-edge-reference 3 3 3 0
```

两者使用相同 raw、adapter、TSACore 和参数，reference 仅禁用本地首报修正，
仍包含轮廓越界修复。输出坐标为原始面板 `2560×1600` 方向，尚未应用接线变换
或桌面旋转。合成测试不能代替真实 Android 全面屏手势验收。

2026-10-23 扩展回归：方向 3、原始面板上边、每 9 ms 移动 3 格时，首个候选距
逻辑边 634 Q8 单位，超过该边 563 的窄带；新增前置边缘证据后，首报距物理上边
由约 218.3 像素降至 0.1 像素，准入时刻不变。240 组合成 A/B 覆盖四方向、四边、
每帧移动 1.5/2/2.5/3/4 格及三种切向速度；12 组超出窄带的屏内起始对照完整保持
上游轨迹。独立证据测试覆盖有效贴边脉冲及 19 种拒绝/复位/时序情况。
这仍不是实机手势验收；速度更高、采样跳过边缘、前置信号太弱或方向不确定时，
不会凭空生成边缘起点。首报补回边界后，下一帧仍使用正常实时坐标，快速滑入可能
出现较大的首段位移；不插入历史点排队，也不额外增加按下或抬起等待。

2026-10-23 实际六次滑入发现另一条漏判：初始候选已经在逻辑边界，但连续两帧的
坐标被夹在该边界，第三帧才明显内移。此前三点方向检查要求前一段也至少移动
50 Q8，因其位移为零而拒绝了补偿。现只允许**起点恰在边界、前一段法向位移为零**
的这种情况，切向漂移仍须小于当前内移量；屏内停顿、反向、突转、弱信号和超时
保持拒绝。既有峰检测/消抖及报告准入没有改变。

记录 `deploy-logs/session-gFvuwGiD/` 包含 2091 帧、六次 DOWN/UP，用户指出第三、
第五次起点偏内。记录时的 strict replay 2091/2091 通过；原版 raw replay 也与
实时输出及候选轨迹逐行相同。修正后离线 A/B 的首点距对应边缘由
`0.2 / 0.2 / 44.9 / 0.2 / 43.1 / 37.1` 变为六次均 `0.2` 面板像素，
178 条输出的帧号、时间、状态、ID、事件和压力逐项一致，28 条坐标受首点及后续
历史/滤波状态影响。第六次的偏移是记录里的额外观察，不改写用户的第三/第五次反馈。
四边的该形态及屏内停顿、反向、突转、超时、弱信号、静止对照已加入回归；
Release 与完整 ASan/UBSan 各 52/52 通过。修改后的程序严格回放旧输出会如预期在
frame 1225 拒绝，不把算法 A/B 当作旧输出一致性通过；实机改善仍需重新滑动验收。

修正已作为 `04f904d` 部署到当前 Linux，仅更新 daemon/replay。安装后 fresh smoke
553 帧全部有效且 strict replay 一致；常驻服务 PID=1789170、NRestarts=0，
877 帧时 bad/stale=0，无真实触点报告。daemon SHA-256 为
`d05908688bfa218805cb43b999fdd508900074c23efe561184b79697ed482d39`，
replay 为 `4a03b8719c7fabfb3612a146c2bf55d52d6e3598f6a1199bb62f46562bb3eb26`。
内核/模块、启动镜像、unit 和 SPI/GPI 运行 notes 核对不变，健康门控无新故障；
自动补读保持默认关闭。备份/验证在 `deploy-logs/edge-plateau-install-Y66EmoWj/`。
没有改写原采集文件；原版程序仍可复核 2091 帧记录时的输出，新的滑动效果待用户验收。

可从现有同步采集离线导出尚未上报的候选轨迹：

```sh
userspace/build/himax-replay --session --touch-trace /tmp/himax-candidates.csv \
    deploy-logs/session-XXXXXXXX/capture.hxst > /tmp/himax-contacts.csv
```

`--touch-trace` 仅用于离线 replay，没有常驻 daemon 日志或热路径开销。输出文件
必须不存在，避免覆盖原始证据；进程必须返回 0，才能将整个输出视为严格同步
验证通过，失败时留下的 CSV 仅是前缀。普通 `--hxst`/native 回放也支持它，但
不会因此获得同步 session 的实时输出等价保证。

每行是 `Process()` 完成并写回的历史触点，包含 epoch/sequence、候选起始时间、
消抖帧数、是否曾上报、最初/当前/首报位置、区域范围、异常/噪声和边缘参数。
`core_id` 是内部 ID，不能直接当成 mapper/uinput ID；同 ID 在释放或 epoch 切换
后可能重用。坐标采用算法 dim1/dim2 的 Q8 传感器单位，逻辑边界同时输出，
不是桌面像素。它不能展示创建候选前就被滤掉的 raw 峰，也不能证明触点来自屏外。
没有候选的帧不写行，已经在当帧移除的历史触点也可能不出现。
`core_time_ms` 是历史单元自身的时间；与输入 `time_ms` 不同的行不能当成该输入帧
新测得的位置，例如算法在 IDLE/校准期间未执行一次完整触点求解时。

先比较候选开始到 `reported_once` 首次为 1 的时间和位移，再对照 contacts 中
的 DOWN。如果首个候选已经在范围外，仍需回看录制内更早的 raw 网格，判断是
检测过滤还是首次采样就已进入屏内；不能只凭这张 CSV 下结论。

`scripts/capture-v4l2-session.sh 90` 现在会在新会话结束后自动生成
`contacts.csv`、`core-touch-trace.csv` 和 `replay.log`，同时保留
`capture.hxst` 与 `.events`；只有严格回放成功才报告会话完成。轨迹导出在离线阶段
进行，不给常驻触控增加文件写入。该脚本会暂时重启 AFE/solver，获得可完整回放的
初始状态；它不是不重置现状的 live 窗口。

实际快滑对照应保持同一屏幕方向和同一条边，先做两次慢速屏外滑入，再做六次快速
滑入，每次明显抬手并间隔约一秒；记下屏内起点出现在哪次。第一轮从边缘中段开始，
避免角落的有意排除条件。对照成功和异常两组的首次 raw 信号、候选起始帧、
首次获准上报及边缘范围；无法证明来源时不直接扩大补偿阈值。

## 构建

需要支持 C++23 的 GCC/Clang、CMake、pkg-config、libsystemd 开发文件和当前内核 build tree。
可选 KDE 方向桥接另需 Python 3 和 `kscreen-doctor`（host tests 也使用 Python 3）。

```sh
git submodule update --init third_party/EGoTouchRev
cmake -S userspace -B userspace/build -DCMAKE_BUILD_TYPE=Release
cmake --build userspace/build -j4
ctest --test-dir userspace/build --output-on-failure
make -C touchscreen-hx83121a-dkms HIMAX_V4L2=1
```

内核需要 `VIDEO_DEV`、`VIDEOBUF2_V4L2`、`VIDEOBUF2_VMALLOC`、`INPUT_UINPUT` 和
DRM panel follower 支持。若本机内核没有 uinput，但 build tree 含匹配的完整源码，
可构建未修改的上游 uinput 模块：

```sh
bash scripts/build-uinput.sh
sudo insmod artifacts/uinput.ko
```

后续内核应直接启用 `CONFIG_INPUT_UINPUT=m`；这个辅助模块不能跨内核版本复用。

## 部署和运行

先做短时采集、服务、watchdog 和打开 MMAP 时解绑测试，完成后自动恢复原模块：

```sh
bash scripts/deploy-v4l2-local.sh --trial
# 延长正常求解测试；原始帧观察采集仍为五秒：
HIMAX_TRIAL_SECONDS=20 bash scripts/deploy-v4l2-local.sh --trial
```

完整部署会安装模块、用户态程序、udev 规则、systemd unit 和 modules-load 配置：

```sh
bash scripts/deploy-v4l2-local.sh
systemctl status himax-touchd
journalctl -u himax-touchd -b
```

部署时先在原 unit 定义下停止常驻服务，再加 runtime mask，防止 udev 抢占临时
采集。不要换回 `mask --now`：本机曾在替换运行中的 notify unit 后拒收其通知，
导致旧服务被 watchdog 终止。正式安装通过短时验证后，会保留临时求解服务运行，
直到 DKMS/dracut 与文件安装结束，再移交给常驻服务；交接前检查临时服务仍 active。
临时试验仍按原流程停止求解服务，进行 watchdog/解绑检查，最后恢复之前的模块。

脚本要求有现存、可恢复的未压缩模块；备份和日志保留在
`deploy-logs/v4l2-时间戳/`。`--trial` 不替换启动时使用的模块或启用常驻服务，
但会按需加载 uinput/videobuf2 依赖。
常驻安装会强制重建 DKMS 缓存，并核对仓库产物、缓存、磁盘模块和运行中 build ID；
失败时恢复原模块、DKMS 源码/注册状态和原服务状态。
安装后可另跑 `bash scripts/test-v4l2-transport.sh`，用真实 ioctl 验证队列 owner、
非法参数、过期 epoch、跨 AFE 重建的一秒 reset 限频，以及 STREAMOFF 后拒绝命令。
测试会暂停服务并执行两次硬件 reset，退出后恢复原服务状态；它不进入普通 CTest。
固件完成握手另用以下 opt-in 测试；测试期间不要触摸屏幕，发现固件 finger 状态会
中止。测试会重建 AFE、显式校准、尝试两个频点并进入 IDLE，退出时恢复原常驻服务：

```sh
bash scripts/test-v4l2-afe.sh          # 全套；当前设备的频点切换检查尚不通过，见文末
bash scripts/test-v4l2-afe.sh --basic  # 只检查校准、清状态、IDLE；明确跳过跳频，不算全套通过
bash scripts/test-v4l2-afe.sh --frequency-only # 不再显式校准；等待 240 个无手指帧后单独检查切频
```

原始 HXRC/HXST、各命令的 epoch/sequence 和观察到的 suffix 状态、内核及服务日志
保留在 `deploy-logs/afe-*/`。只在命令之后采集到的帧确认状态，不能用队列中的旧帧
充当完成回执；读回成功与固件完成分别记录。扫描率命令目前只能验证传输，adapter
固定报告 120 Hz，不应据此声称验证了真实扫描率。该录制不是同步求解 session，
不宣称严格输出等价；失败录制仍保留供离线分析。

需要同时查看现有邮箱读回时，优先用临时诊断模块，测试后自动恢复当前安装的模块：

```sh
HIMAX_TRIAL_AFE=basic bash scripts/deploy-v4l2-local.sh --trial
HIMAX_TRIAL_AFE=full bash scripts/deploy-v4l2-local.sh --trial
HIMAX_TRIAL_AFE=frequency bash scripts/deploy-v4l2-local.sh --trial
```

该选项仅允许用于 `--trial`，会自动启用 `HIMAX_AFE_DIAGNOSTICS=1`，在模块加载时打开
`afe_mailbox_debug`，保存 `afe-test.log`、`afe-kernel.log` 和 `afe.hxst`；随后关闭日志。
实机检查失败会返回非零并恢复原模块，不把跳频失败改成安装成功。基础检查通过后还会
继续普通 solver/watchdog/解绑试验再恢复。手动构建诊断模块时，`afe_mailbox_debug`
默认仍为 false，仅 root 可改；它只打印协议本来就会读回的 16 字节，没有额外 SPI
访问或逐帧回调。默认 `HIMAX_AFE_DIAGNOSTICS=0`，连日志与模块参数都不编译进普通模块，
无需内核启用 dynamic debug。该编译开关与 legacy 的全速原始帧诊断开关相互独立。

系统 PM 的状态检查有 host 回归测试，但屏幕 DPMS 测试不等于真实系统休眠验收。
启动校准后可能有短暂无效原始帧；安装检查明确记录并最多容忍每个 epoch 的前四个
无效帧。一旦出现首个有效帧，后续任何坏帧仍使安装检查失败；全坏、连续五个启动
坏帧和截断文件也失败。运行服务的连续五坏帧规则独立负责请求 AFE reset。

手动运行时不要同时启动 systemd 服务：

```sh
sudo userspace/build/himax-touchd
# 有多个控制器时显式指定：
sudo userspace/build/himax-touchd --device /dev/video2
```

设备通过 QUERYCAP 自动识别，不依赖固定 video 编号；安装 udev 规则后另有
`/dev/himax-raw` 别名。面板参数默认从 OTP project ID 选择，失败时明确提示使用上游
`W273AS2700` fallback；实测为其他面板时可用 `--project` 显式指定。
OTP 读取遵循原版 safe-mode、LPWU、DD access 解锁和 BB 恢复流程；
本机已实际读出 `W273AS2700`，并非仅按 fallback 选择参数。
若服务在屏灭时启动，先保持 `project=pending-OTP`，等亮屏后首次运行 epoch
完成硬件识别，再选择参数；不会提前把空 OTP 当作已确定的 CSOT 面板。
一次采集会话内不允许面板参数变化，身份变化会退出并由服务重新建立会话。

可在服务运行时只读查询，不占用采集队列、不初始化硬件：

```sh
sudo /usr/local/bin/himax-touchd --info
```

当前 live ABI 为 v2，内核与 daemon 必须配套升级。INFO 包含固件描述的坐标变换：
bit 0 为 X 翻转，bit 1 为 Y 翻转，bit 2 为交换 X/Y。uinput 先在原传感器坐标轴上翻转，
再交换轴，与 Linux touchscreen helpers 保持一致。本机值为 `5`，交换后的输入量程
为 `15999×25599`，libinput 物理尺寸约 `167×267 mm`。桌面旋转与触控接线变换分开，
但服务会以低频读取屏幕方向并传入 TSACore，供握持/边缘策略使用。

方向值是 TSACore 的 `0/1/2/3`（0/90/180/270）。KDE 下，在执行过
`sudo cmake --install userspace/build` 后，由桌面用户启用独立桥接：

```sh
systemctl --user daemon-reload
systemctl --user enable --now himax-screen-bridge.service
```

桥接的后台线程每秒在桌面会话中执行一次 `kscreen-doctor --json`，只选择已启用且
已连接的 `DSI-1`，通过 Unix datagram 发布方向。桥接主线程另接收启动请求，只从
最近三秒内成功查询的缓存应答，不会因请求启动 KDE 查询。查询失败或找不到面板时
清缓存；仅因触控服务未运行而发送失败时保留缓存。

主服务在打开采集设备、启动 AFE 前就建立方向接收器，向当前 `seat0` 用户的桥接发
一次非阻塞初值请求；硬件初始化期间即可排队接收应答。之后每 250 ms 排空有界
消息队列，只接受 root 或 logind `seat0` 当前活动用户的内核验证凭据。主服务没有
连接桌面 D-Bus，也不需要放宽 `ProtectHome` 或增加文件读取权限；每秒重发仍可覆盖
服务重启或启动时没有缓存的情况。触控 daemon 默认仍为单线程。
KScreen 的 `1/2/4/8` 分别映射为 `0/1/2/3`，镜像模式拒绝猜测。

桥接请求端点为抽象 Unix datagram 地址 `himax-screen-bridge-<UID>`，请求体严格为
`HXSR 1\n`。请求方必须是 root 或桥接自身 UID，且源地址必须是固定触控接收端点
`himax-touchd-screen-state`；回复只发往该端点。两端均拒绝截断报文和意外文件描述符，
并关闭 `recvmsg` 已接收的文件描述符。桥接每批最多处理 16 条请求，批次间隔至少
250 ms，防止请求流反复触发处理；方向缓存回复与周期发布串行，避免旧回复越过新方向。

`--rotation N` 固定值优先；其次为 `--rotation-file FILE`/
`HIMAX_SCREEN_ROTATION_FILE`，内容可为 `0..3`、`90/180/270`、`rotation=...`，
或显式带标签的 `kscreen=1|2|4|8`。文件源不存在/错误时不改读 framebuffer；
`fb0/rotate` 并不代表 Wayland 桌面方向。文件限长 63 字节，拒绝 FIFO/设备/软链接。
未获得方向时保留上游默认值，日志显示 `screen_rotation=-1`；桥接暂失时保留最近方向。
初值请求不等待应答、不暂停触控；桥接未运行、缓存过期或应答到达较迟时，首批帧
仍可能使用上游默认值，不能把握手当作所有启动场景下首帧方向必定可用的保证。
其他桌面或 Android 的显示桥接可原子更新显式文件，但尚未提供对应生产者。

方向更新只影响 TSACore 握持/边缘区域，不改变上报坐标，也不人为抬起触点、不清
mapper、不复位 AFE/校准/基线。宿主方向在初始化和 `MsReset` 内部立即应用，
保证当帧后续检测和握持处理也使用正确方向。手写笔仍未启用，方向输入与笔状态
通道分离；上游 submodule 保持不变，构建适配及其校验见本文开头。

手动运行时可指定方向：

```sh
sudo userspace/build/himax-touchd --rotation 3
sudo userspace/build/himax-touchd --rotation-file /run/himax-screen-rotation
```

服务用三秒 watchdog 监控整个求解循环，进程退出或被终止时关闭 uinput，销毁虚拟设备。
屏灭事件立即释放所有 slot；epoch、sequence 和时间检查禁止屏灭前队列帧重新生成触点。
250 ms 没有有效新帧时释放已上报触点。该超时是故障保护，不加入正常逐帧防抖。
运行状态下四秒没有新帧时服务以失败退出，由 systemd 重启并重新初始化 AFE；
该期限容纳原版 master IDLE 约三秒的间隔。屏灭期间不触发产帧超时。
systemd 在故障退出后等待两秒重试，不设十次启动后永久停止的速率限制。
硬件一直不可用时仍会持续重试，具体失败原因保留在 journal。

可选的 KDE Wayland 面板测试会短暂熄灭唯一的 `DSI-1`，在屏灭期间重载已安装模块并
启动服务，再亮屏验证 OTP、同一服务进程和实际算法处理。脚本要求仓库模块与已安装
模块一致，使用独立的二十秒亮屏定时器及退出恢复操作：

```sh
bash scripts/test-v4l2-panel.sh
```

它测试 DPMS 与屏灭冷启动，不会执行系统 suspend，也不能替代实际滑动验收。

## 正常触控采集和严格回放

`--record FILE.hxst` 可在正常求解模式使用：触摸输入和 AFE 闭环继续工作，另一个
后台线程保存 raw 和宿主状态。只有启用录制时才分配 128 个事件的 SPSC 队列并复制
诊断数据；正常默认服务不分配队列、不创建写盘线程、不计算输出摘要。

录制创建两个新文件，不覆盖已有文件：

- `FILE.hxst`：原有 HXRC v1 面板头及未经修改的 HXST 帧；
- `FILE.hxst.events`：HXSE v4 power 侧车（兼容读取旧 v1/v3），保存每帧使用的方向、求解结果摘要、AFE 请求及
  执行退出状态、panel 生命周期和超时释放事件，以及完整性尾记录。

输入线程不等待磁盘：队列满或后台写入失败会禁用录制并记录错误，触控继续。
结束时检查尾记录和 raw/元数据数量；丢记录、截断、后台写失败、提前异常退出和
文件配对错误不能被当作成功。退出时先释放输入，再等待后台线程排空。
这些文件通过普通 write/flush 保存，没有逐帧 fsync，不保证突然断电后的持久性。

先从服务启动时开始录制，才能重建相同的初始算法状态：

```sh
bash scripts/capture-v4l2-session.sh 10
```

脚本使用当前仓库构建产物，保存二进制哈希和源码版本，创建独立的 `deploy-logs/`
目录。它在正常/失败/中断时恢复此前运行的常驻服务，随后严格回放；采集期间仍有
触摸输入。需要手动运行时：

```sh
sudo systemctl stop himax-touchd
sudo timeout --signal=TERM 10 userspace/build/himax-touchd \
    --record /tmp/capture.hxst
sudo systemctl start himax-touchd
userspace/build/himax-replay --session /tmp/capture.hxst > /tmp/contacts.csv
```

`timeout` 到时通常返回 124；采集是否完整以 daemon 的录制错误统计和
`--session` 回放结果为准。上面的命令会重建 AFE/求解器，不能用来保留已经运行中的
故障 baseline；当前还没有给常驻进程提供运行中开启录制或状态快照接口。
录制从第一个实际面板身份确定的帧开始，因此屏灭期间启动也不会使用猜测的面板头。

严格回放按录制方向、panel/epoch 事件和命令成功/失败结果驱动同一 solver，逐帧比较
report、AFE status、ctrl、触点输出摘要和 AFE 请求；任何不一致都返回失败。
成功时输出 `live_output_verified=1`。这证明该录制在当前二进制下可重现，
不证明触控效果正确，也不证明已经与原厂 DLL 等价。摘要含浮点位表示，跨编译器/
算法版本回放可能出现差异，应保留构建版本；新旧求解器间的差异需要单独分析。
`--session` 禁止覆盖面板或方向；若有意改变算法条件，使用普通 raw 回放：

```sh
userspace/build/himax-replay --hxst /tmp/capture.hxst > /tmp/contacts.csv
# 普通 raw 回放不使用 .events，不校验现场输出：
userspace/build/himax-replay --rotation 3 --hxst /tmp/capture.hxst > /tmp/contacts.csv
```

观察模式仍独占 V4L2 队列，不创建输入设备，也不执行算法请求的 AFE 命令。
因此观察期间触屏不可操作，不等价于正常服务下的 AFE 闭环。需要保持原有行为时使用：

```sh
sudo userspace/build/himax-touchd --observe --record /tmp/observe.hxst
```

观察录制也有侧车，未执行的命令结果为 `-1`，不能冒充固件已执行成功。
只有原版 solver 请求上报时才写入 uinput；无触点的正常帧不会产生逐帧空快照。
正常服务退出时输出 solver 平均/最大用时、IRQ 到 uinput 的 p95/p99 上界及最大用时，
以及 IDLE/校准完成/切频完成状态帧数和各类 AFE 命令的请求、成功、失败次数。
需要查看当前累计统计而不中断触控时：

```sh
sudo systemctl kill --kill-whom=main --signal=USR1 himax-touchd.service
journalctl -u himax-touchd.service -b -n 30
```

命令成功计数仅表示 ioctl/邮箱传输成功，固件完成状态仍须结合后续帧判断。
`touch_reports=0` 时不会输出 IRQ→uinput 时延，因为没有实际触点报告样本。
该时延不包括控制器扫描之前的硬件时间，也不包括 compositor/application 响应。

也支持旧诊断 trace 和连续裸帧：

```sh
userspace/build/himax-replay --trace event-trace.bin > /tmp/trace.csv
userspace/build/himax-replay event-stack.bin > /tmp/raw.csv
userspace/build/himax-replay frames.bin W273AS2700 5060 > /tmp/master.csv
# 安装检查使用的有界启动瞬态容忍；保留记录及拒绝计数：
userspace/build/himax-replay --allow-startup-invalid --hxst /tmp/capture.hxst
```

HXST/旧 trace 使用记录的单调时间戳；裸帧按 120 Hz 重建，不能用于真实时延测量。
raw 录制文件使用 32 字节 `HXRC` 容器头，保存 project ID 和固件坐标标志，后接
5184 字节的 HXST 帧。回放自动使用记录的面板参数；显式传入不同 project ID 时，
会输出覆盖提示。仍兼容旧的无容器头 ABI v1/v2 HXST；旧文件没有面板身份，
回放其他面板时应显式指定 project ID。文件格式详见架构说明。
CSV 坐标单位为面板原始方向的显示像素；uinput 使用其十倍量程，保留亚像素信息。
native master 只前补三个运输字节，不改变原始矩阵、suffix 和 checksum。
5132 字节 payload 容量中，仅前 5060 字节属于 master，checksum 覆盖 `[2, 5060)`。
新模块一次读取完整 master，`payload_size=5060`，尾部 72 字节为零 padding；
旧模块/旧记录为 `payload_size=5132`。新 reader 接受两种长度，按实际长度回放，
新格式尾部非零时拒绝。旧 daemon 不接受新模块，更新时先安装新版用户态；
结构/ioctl 保持 ABI v2，不代表新模块能被旧 daemon 使用。
suffix 中的 `0xbb` 是校准完成状态，通过 ClearStatus(8) 确认，不能作为每帧固定 magic。
帧有效性按固定 Windows `CheckFrame`/adapter 的 checksum body 判定，不额外要求
前两个 native 字节为 `5a a5`。长度先检查，全零与 `0xec` 填充仍拒绝；旧版日志中的
`sync` 是历史额外检查，新版不再产生该分类。旧 session 的跨版本严格回放仍须留意
行为变化：如果原版跳过过 checksum 合法的非固定头帧，新版会处理它，严格回放将
明确不匹配，不会伪造当时输出。raw 回放仍可用于比较。

## AFE 初始化失败的异步退避

当前测试内核已部署的普通模块按固定上游 `DeviceRuntime::BringUpBackoff()` 的顺序采用
200 / 500 / 1000 / 1500 / 2000 ms，之后封顶 2000 ms。每次 work 只做一次完整
初始化，不再持有 `op_lock` 连续做三轮 reset/init；退避使用 delayed work，等待期间
不持锁。协议内部的有界轮询/短等待没有改变，不能说整个初始化过程已可即时取消。

`VIDIOC_STREAMON` 成功只表示流请求已接收（原本屏灭启动就有这种语义）；若初始化
失败，发布 stopped epoch，IRQ 保持关闭，排队重试。消费端等待 running 事件与实际
raw 帧后才处理，不能以 service active 或 STREAMON=0 作为触摸已恢复的证据。
实际生产者进入 running 后仍受原四秒进度超时保护，未放宽为永不检查停滞。

每次回调都重新检查 streaming、panel prepared/enabled、system suspend、shutdown
与 initialized 状态；成功/STREAMOFF/屏灭/休眠/关闭会清零退避计数。设备移除仍通过
ENODEV/队列错误断开，不会无限保留已移除设备的流。健康流收到迟到任务时不关 IRQ。
每次硬件尝试推进 epoch，旧帧和命令失效，但外部 reset 命令的跨 epoch 限频不清零。

这只移植社区 Windows 重写版的“一次尝试 + 异步退避”政策，不等于完整移植原厂状态机：
Windows 的 master/slave 0x13 bus-ready 前置探测、每五次失败重开 THP 句柄，仍没有被
冒充为 Linux 等价操作。没有新增 slave/笔读取，也绝不因此卸载或重绑 SPI/GPI 控制器。
OTP、calibration/rate best-effort、硬件单次初始化事务保持原策略。当前源码的清帧
已改为下面说明的异步 producer cleanup，已安装模块仍是旧的定时三次直读实现。

### IRQ producer 启动清帧

`himax-v4l2-startup.h` 将 consumer-ready 与硬件初始化完成分开：打开 IRQ 后
做一次初始 producer 读取，随后由正常 IRQ 消耗剩余的启动读取尝试。每次最多等
200 ms，超时才补读；不持锁等待 IRQ、不循环固定睡 8 ms 直读。三次尝试结束才
校准/设速并发布 running，启动数据不送入 TSACore；最后一次超时后的成功 kick
保留为第一份正常流数据。停止/休眠取消状态，命令与 charger 写入在清帧期间关门。
无数据/失败仍在三次有界尝试后完成 best-effort 清理，后续缺帧由既有 supervisor
处理；没有把“清理结束”当成有效触控或校准完成。

新增独立 poll-completion 时间，防止 IRQ 在主动 SPI 读取中途排队后再次读同一包；
不改 HXST 的采集 timestamp。普通路径无 baseline/跟踪代码变化，也不默认打开
运行时实验恢复。完整验证与移植边界见
[SPB 启动清帧审计](../docs/windows-spb-backend-audit.md#启动清帧的可执行参考)。

最终 52 项 Release、52 项完整 ASan/UBSan、legacy host tests 和 checkpatch 通过。
服务器已构建匹配测试内核的普通模块，候选为
`artifacts/startup-drain-himax1/himax-spi.ko`，SHA-256 为
`648ef91a7b050d6a7be2e8f71013f2e5b8bb9af165d165c0edb623a36a52de59`。
构建完成时尚未部署此模块；当时用户态仍为前述边界修复版本。随后的试载与正式
安装记录见下文，不用离线测试冒充实机完成。

随后已完成普通候选的两轮临时试载，**试载后仍恢复原已安装模块**：

- 869 帧 fresh daemon 会话全部有效、零丢失，strict replay 输出一致；
- 新的 opt-in `himax-startup-hardware-test DEVICE NEW.hxst` 只在手动执行时访问
  设备，记录 fresh start 和三次显式 reset 的 raw/epoch/首次有效数据及校准完成。
  两轮分别 188/187 帧全有效。它不运行 solver，不用 raw 回放冒充实时求解一致；
- `himax-startup-hardware-test --cancel-start DEVICE` 在只见 stopped epoch 时
  STREAMOFF，再观察 350 ms 没有迟到 running；kernel 清帧完成日志也未出现。
  后续重开并三次 reset 正常。取消在 STREAMON 返回后，不能称事务中途取消；
- 基础 AFE 确认校准/清状态/IDLE，跳频不测，rate0 仍仅传输证据；
- 正常路径共十次启动清理均为三包消费、零超时。没有主动停 IRQ 或制造 DMA/SPI
  错误；睡眠、DPMS、手指/掌压在场与实际手势仍未验收；
- 两轮只换 Himax 客户端，不重载父控制器；无新 kernel fault。旧模块、正常服务、
  运行与磁盘程序以及两份 initramfs 全部恢复/核对，不持久安装候选。
  详细记录见 [SPB 启动实测](../docs/windows-spb-backend-audit.md#普通候选的真实启动reset-和启动中取消)。

### 两帧 raw 保留队列

`e962070` 将内核分配回调和 Capture 的申请统一为两帧。队满仍读 SPI、丢新交付，
旧包和原始 timestamp 保留，序号间隔可见；不是覆盖最旧包的 latest-only 策略。
这样对齐已审计 Windows SPB FIFO 的保留容量与 drop-new 方向，不增加每帧分配。
ABI/记录尺寸不变；勿设置 `vb2_queue.max_num_buffers=2`，该内核会 WARN。

53 项 Release、完整 53 项 ASan/UBSan 和服务器模块构建通过。临时实机试载确认
REQBUFS 请求 1/4/8/32 均返回 2；消费者停 120 ms 后仍取出 sequence 1/2，随后
有效数据为 sequence 16，同 epoch，13 个被丢的新交付有序号证据。正常会话另录
878 帧并通过严格输出回放，启动取消及三次 reset 也通过。详细测试和边界见
[SPB 两帧队列审计](../docs/windows-spb-backend-audit.md#两帧保留容量与队满行为)。

本轮试载退出时尚未持久部署：已恢复原 module/daemon/服务，两份 initramfs 未改。
候选为 `artifacts/two-buffer-himax1/himax-spi.ko`（SHA-256 `efa7362d…`），
本机 Release daemon 为 `f1725ef1…`，都不代表当前安装版本；证据和回退备份在
`deploy-logs/two-buffer-trial-m6JxStpd/`。空载会话不能证明有触点/游戏/长时负载表现。

### 2026-10-24：启动清帧和两帧队列已安装

完成前述试载后，已将普通构建安装到当前 `7.1.8-gaokun3-himax1`：

- Himax module：SHA-256 `efa7362dee86c51bd6f0ff264dd22e3924474bf9b73049919ef0728f2e14f2f0`，
  build ID `0f0a1f302b188071dbc329a35f4b4509fe98a4ed`；
- daemon：SHA-256 `f1725ef1575993cc8d013be460fdcb47b3ee1d4a029fc63f8d71bab2627b89b3`，
  仍含此前边界 plateau 首点修复；replay/算法源码没有因队列改动再次改变；
- 常驻 PID=2275950、epoch=1，878→919 帧时 bad/stale=0，IDLE 帧 5→46，
  NRestarts=0，无触点、额外 reset/stall 或自动 kick；
- 自动 rearm 仍默认关闭，无 live-capture 参数或管理线程，没有部署诊断注入模块；
- `/boot` 与 ESP 测试项 initramfs 均为 SHA-256
  `f9c9d15aa8333b827c30447ad729579e7eca249fa569bd57a2b3efa0839540a2`，
  解包 Himax/GPI/SPI/uinput 与磁盘逐字节相同；
- Image、DTB、BLS entry、unit、loader.conf 和 SPI/GPI 运行 notes 未改，
  bootctl 前后相同。没有重启、自动睡眠、改默认启动项或部署 Android；
- 旧模块、daemon/replay、两份旧 initramfs、安装脚本及 kernel health 证据保存在
  `deploy-logs/two-buffer-install-jVfctxCC/previous-*` 及同目录，不占 ESP 备份空间。

当前会话已使用新版，无需重启生效。下次仍需选择 **Himax SPI/GPI test** 项；默认
启动项保持旧内核。更新镜像未冷启动验收，真实系统 PM、触点/过载释放、游戏与
长期性能仍待测试；空载统计只证明这段正常帧流。

诊断构建（`HIMAX_AFE_DIAGNOSTICS=1`）另提供仅模块加载时配置的
`afe_start_failures=0..8`（默认 0、只读参数）。probe 将预算复制到每个设备；每次
AFE 尝试在 IC 检测/SPI 之前消耗一次并返回 EAGAIN，用于检验真实 workqueue 的
退避与取消。STREAMOFF/后续 STREAMON 不重新填满预算，普通构建无参数、计数和
分支。这不是 SPI 返回值故障注入，也不证明真实控制器错误恢复；不要在日常使用
或已故障的现场临时加载它，加载/卸载模块会丢失原现场。

## 不重置当前触控的运行中窗口录制

代码提供可选 `--live-capture-dir PRIVATE_DIR` 和 `--live-capture-seconds 1..120`
（默认 10 秒）。未配置时不创建录制管理线程/队列，不逐帧复制或计算诊断摘要。
配置后不主动录制，收到 SIGUSR2 才开始，再发一次提前停止；每次结束由后台线程
排空/关闭，上一窗口尚未关闭时拒绝新请求。主触摸线程不创建文件、不等待落盘。

`c00844e` 已安装并完成两次实机窗口验证，**当前默认仍关闭录制**。旧 daemon 的
SIGUSR2 可能是终止信号，不能直接对旧版本发送。辅助脚本读取运行进程参数，未配置
该选项、服务停止或 PID 改变时不发信号，也不会自动停止/重启服务。

部署新版用户态后，可选择启用一次（这次服务重启会重建 AFE/solver；后续窗口不会）：

```sh
# 示例 drop-in 随 cmake --install 安装在 share/doc/himax-touchd 下，不会自动启用。
sudo install -Dm644 /usr/local/share/doc/himax-touchd/himax-touchd-live-capture.conf \
  /etc/systemd/system/himax-touchd.service.d/live-capture.conf
sudo systemctl daemon-reload
sudo systemctl restart himax-touchd.service

# 之后发现异常时：不停止服务、不抢占 V4L2 队列、不校准、不重建 baseline。
bash scripts/capture-v4l2-live.sh
journalctl -u himax-touchd.service -b -n 40
```

systemd 的 `StateDirectory=himax-touchd-captures` 提供
`/var/lib/himax-touchd-captures` 的私有写入目录，模式 0700；兼容现有
`ProtectSystem=strict`，无须放宽整个文件系统权限。手动启动时须预先提供由进程
用户拥有、group/other 不可写的目录；最终路径为 symlink 时拒绝。每个窗口使用
独立 `live-XXXXXXXX/capture.hxst`，已有证据不覆盖。10 秒/120 Hz 原始帧约 6.2 MB，
没有自动清理旧证据，磁盘不足时窗口失败但触控继续。

保存内容包括当时的 raw、epoch/sequence/时间戳、面板身份与坐标标志，以及旁文件
中的方向、生命周期、输出 digest/status/ctrl 和实际 AFE 命令/结果。它从已有
算法状态中途开始，文件头明确为 **`HXSE 4 live-no-checkpoint`**，即使正常收尾
也不是“完整可重现的 solver 会话”。`himax-replay --session` 会明确拒绝，不能因为
平坦 raw 恰好给出相同空输出就声称现场已严格复现。可用普通 raw 回放研究矩阵：

```sh
sudo /usr/local/bin/himax-replay --hxst \
  /var/lib/himax-touchd-captures/live-XXXXXXXX/capture.hxst > /tmp/raw-replay.csv
```

该命令从新的算法状态求解，输出不是现场触点的精确重建；真实输出摘要/动作保存在
`.events`。需要严格逐帧验收时仍使用 `--record` 的 fresh 会话。运行窗口无历史预录，
也没有实现 checkpoint；暂停/重启正在故障的驱动来录制会丢现场，不是替代方案。

只有后台日志中的 `window saved` 表示窗口正常收尾；辅助脚本的“已发送请求”
不是采集成功证明。无帧、目录/写入失败、队列溢出或未正常停止都不能报告成功。
诊断开启时仍有复制、摘要和线程唤醒开销，未做实机时延基准，不能宣称零性能成本。

## 恢复旧内核算法

旧内核算法为单独构建选项，不与用户态求解同时运行：

```sh
HIMAX_V4L2=0 bash scripts/deploy-local.sh
# 需要旧诊断 sysfs/ring 时：
HIMAX_V4L2=0 HIMAX_DIAGNOSTICS=1 bash scripts/deploy-local.sh
```

该路径会停止/禁用用户态服务，并把 DKMS 源码的默认值保存为 legacy。
新 V4L2 路径没有旧 `algo/` 和 `event_trace` ABI，现有诊断 APK/调参 UI 尚未迁移；
Android init/SELinux、内核配置和设备映射也需要独立集成。

## 验证边界

离线测试覆盖 native checksum、四种面板 ID 的参数选择、完整 TSACore 初始化及状态
转换、Type-B ID 生命周期及八种坐标变换、epoch/旧帧丢弃、产帧超时、HXRC/HXST/HXTR
格式、截断文件及写入失败，以及 OTP 完整协议和逐次总线操作故障注入。
已有真实 512 帧历史 trace 可完整回放。
这不证明掌压亮屏、快速滑动、多指和长期运行已经通过；不能据此宣称解决原有故障。

实时内核 transport 不做 raw checksum/峰值/基线判定；SPI 读取失败不会投递半帧，
连续三次失败触发受限重启。用户态校验失败或 SOS 请求通过版本化 V4L2 ioctl 申请恢复，
不提供任意寄存器写接口。完整接口、拷贝和故障模型见
[V4L2 架构说明](../docs/touch-offload.md)。

2026-10-22 本机 `7.1.8-gaokun3+` 验证结果：

- Release 与 ASan/UBSan 八组测试通过，旧算法 host regression 和 OTP host tests 通过；
  sanitizer 使用 `detect_leaks=0`，未验收上游进程全局分配的退出泄漏；
- 旧 512 帧 trace、新版 HXRC/HXST 与旧版无头 HXST 回放通过；
- 实际 MMAP 采集、正常服务创建/销毁唯一输入设备、SIGSTOP 后 watchdog 终止、
  保持 fd/MMAP 时解绑与重新绑定通过；
- 本轮五秒观察采集 518 帧，首帧 sync 无效，其余 517 帧通过；
  观察模式 solver 平均 180 µs、最大 1935 µs；
- OTP 实际读出 `W273AS2700`、slot `0x0f18`、revision `0x46`、E2 `0x00`；
- 正常求解闭环观察到校准完成状态经 ClearStatus(8) 后消失，以及进入 IDLE；
  清状态、IDLE 命令各一次且传输成功，没有误触发四秒产帧超时；
- 常驻安装已通过，`himax-touchd.service` 已启用；唯一 uinput 设备被 libinput 识别为
  `167×267 mm` 的 touchscreen，INFO 为 ABI v2、坐标标志 `5`，
  运行中 build ID、磁盘模块和 DKMS 缓存一致；
- 已安装的受限 systemd unit 验证通过，SIGSTOP 后三秒 watchdog 杀死进程，
  服务自动重新初始化 AFE，并恢复唯一输入设备；
- 屏灭冷启动测试通过：屏灭时 project ID 为空，服务等待 OTP；亮屏后同一进程
  识别出 `W273AS2700`，处理 168 帧、坏帧和过期帧均为零；
  solver 平均 161 µs、最大 975 µs。该样本没有触点报告，不能计算实际触控上报时延；
- ABI v2 试用后恢复 ABI v1 的回退已实际验证。曾发现卸载模块连带卸载 vb2 依赖、
  导致升级及回退加载失败；已改为在卸载后分别加载目标模块依赖，并核实实际绑定和
  服务状态后才宣布恢复成功。试用期间临时屏蔽常驻 unit，避免旧 daemon 抢占队列；
- 安装记录及旧模块/DKMS/配置备份：
  `deploy-logs/v4l2-20261022-233928/`；
  面板测试：`deploy-logs/panel-20261022-234330/`。Android 未部署。

实际 suspend/resume、掌压亮屏、充电切换、实际触点与显示的映射、快速滑动、
多指和长期稳定性仍未验收。不能把无触点处理耗时当作触控手感、接触延迟或满负载基准。
早期首次加载曾因错误设置 vb2 最大队列长度触发 warning 并拒绝 probe，随后已修正；
该失败不计为成功的实机验证。系统此前另有 DPU vblank/commit timeout 日志，
现有证据不能将其归因于触控，也不能据此宣称整个内核没有错误。

2026-10-23 屏幕方向增量验证：

- Release 和 ASan/UBSan 各 15 组测试通过，sanitizer 仍使用 `detect_leaks=0`；
- 四种初始方向均覆盖初始化、校准标记、IDLE、AFE epoch，以及合成手指持续按住时
  改方向；合成内区触点保持 ID，没有额外 UP。这不替代真实边缘握持测试；
- 独立 Unix socket 实测有效消息、最新值、非法/截断消息、固定值优先；
  root 运行另外覆盖降权子进程的真实凭据拒绝，不依赖伪造 UID 字段；
- 历史 512 帧真实 trace 在四种固定方向下可回放；各方向重复回放结果一致。
- 本机只更新用户态程序和方向桥接，未重新加载内核模块。两个服务已启用/运行，
  journal 确认 `kscreen=8 → screen_rotation=3 source=desktop-bridge`；
  新进程累计 877 帧，bad/stale 均为 0，`NRestarts=0`。该样本没有实际触点，
  不能用于判定触控手感或报告时延；
- 更新前的程序备份位于 `deploy-logs/userspace-20261023-g9XD1d/`。

当时这些测试不覆盖复位帧内部的瞬时方向值：旧适配层只在帧前后恢复方向，
复位仍可能在当帧中途清零。下文的复位内部方向修复补充了直接状态和握持区域测试，
但仍不能声称已与 Windows 宿主逐帧等价。

2026-10-23 正常闭环录制增量验证：

- `make check` 通过；Release 与 ASan/UBSan 各 16 组测试通过，未启用 leak 检查；
- 新测试包括持续合成触点、方向/charger/校准转换、命令失败和反馈、五次坏帧后的
  reset/new epoch、SPSC 并发与容量上限、实际后台写失败、缺失/伪造尾记录、
  raw/元数据配对错误、输出摘要篡改及 panel-off 后的非法旧帧；
- 本机正常闭环录制 873 帧、观察模式 904 帧，均为 `recording_error=0 dropped=0`，
  严格回放全部一致。正常模式清除了 calibration-done 并进入 IDLE；观察模式的
  904 帧均保留 calibration-done，符合不执行命令的设计；
- 最终采集脚本另外验证 548 帧，严格回放一致；SIGTERM 中断测试确认采集子进程
  退出、原常驻服务恢复。记录位于 `deploy-logs/session-20261023-kj9dYT/`、
  `deploy-logs/session-aIoHbNTN/` 和 `deploy-logs/session-CuChHiDX/`；
- 实机样本没有实际触点，不能代替真实快速滑动、多指、握持或掌压亮屏验收。
- 用户态更新已安装到本机，内核模块未重载。默认服务为单线程、没有开启录制，
  处理 911 帧时 bad/stale 均为 0，`NRestarts=0`，方向为 3；
  安装前备份为 `deploy-logs/userspace-session-20261023-KEK1N5/`。

2026-10-23 系统 PM 与硬件恢复增量验证：

- 系统 suspend/freeze/poweroff 有独立状态，不依赖面板通知是否及时到达；所有运行期
  硬件入口共用状态检查。panel enabled 不能绕过系统休眠，重复初始化任务也不会再
  关闭正常采集的 IRQ；停止与 shutdown 不主动发送 sense-off 或拉 reset；
- 充电的物理状态与固件确认状态分开保存；查询或写入失败会延迟重试，硬件重建后
  重新确认。host tests 覆盖生产状态检查的全部 64 种 power 组合、恢复顺序及写失败
  状态，但未对真实 SPI 注入充电写失败，也未做物理拔插验收；
- `make check` 通过；Release 与 ASan/UBSan 各 17 组通过，仍未启用 leak 检查；
- 真实 ioctl 测试通过：非法参数 `EINVAL`、非 owner `EBUSY`、过期 epoch/
  STREAMOFF 后 `ESTALE`。reset 后 387 ms、已进入新 epoch 时，第二次请求正确
  返回 `EAGAIN`；超过一秒后成功，采集 epoch 为 `7 → 8 → 9`；
- 试用的采集、watchdog 和打开 fd/MMAP 时解绑通过；屏灭冷启动测试通过，灭屏时
  OTP 为空，亮屏后同一 daemon 获得 `W273AS2700`。最终常驻服务处理 880 帧，
  bad/stale 均为 0，`NRestarts=0`，方向为 3；该样本没有实际触点；
- 本机已部署新版模块。仓库产物、安装文件和 DKMS 缓存 SHA-256 均为
  `c27fcdacdd7591d28abad65a7b32b21fc33fa79bf29449665060b3dd6c7bc946`，
  运行中 build ID 与安装文件均为 `08debfe4e179a3a67f53fa4f8d4b870f8f27704b`；
- 备份：`deploy-logs/v4l2-20261023-020055/`；真实命令测试：
  `deploy-logs/transport-iTfKxv9b/`；屏幕测试：
  `deploy-logs/panel-20261023-020435/`。

本轮没有执行真正的系统 suspend/resume 或 hibernation，不能用状态测试或 DPMS
结果替代这些验收。Android 仍未迁移/部署；真实手势、多指、掌压亮屏和长期表现
仍需单独采集并验证。

2026-10-23 复位内部方向增量验证：

- 宿主方向不再只靠 `Process` 前后设置。两处经过完整源文件哈希校验的构建适配，
  在初始化/笔默认状态应用后及 `Sys::Init` 清零方向时立即应用当前求解调用的方向。
  submodule 仍固定在 `4d4f53d` 且工作树干净；接口、布局和参数表没有变化；
- Release 与 ASan/UBSan 各 18 组通过，sanitizer 使用 `detect_leaks=0`。
  直接调用实际 `Sys::Init`、`MsReset` 和握持区域生成函数，覆盖四方向、嵌套作用域、
  线程隔离、无宿主方向的上游默认行为，以及合成持续触点改方向时保持 ID；
- 构建测试覆盖源文件漂移/缺失时拒绝配置、未修改原始文件，以及无变化配置不重写
  生成文件。更新上游版本时必须复审这两处适配；
- 512 帧历史真实 trace，在未指定方向和固定方向 0 两种模式下，修改前后的完整
  CSV 均逐字节相同；这只证明该样本下的行为保持，不能推广为所有输入的等价证明；
- 本机更新了 `himax-touchd` / `himax-replay`，没有重载或修改内核模块。普通录制
  873 帧、同一个录制进程经历 DPMS 关屏/亮屏的录制 1105 帧，均为
  bad/stale/dropped/error 全零，严格回放一致。后者 AFE epoch 从 9 到 12，
  清校准状态命令两次成功；恢复后 230 帧的宿主方向均为 3；
- DPMS 样本启动时有 29 帧尚未收到桌面桥接，记录为 `rotation=-1`，按上游默认值
  处理；之后 1076 帧为 3。这次修复保证的是已知方向在复位内部不丢失，没有解决
  桥接首条消息到达前的初值空窗；
- 两份录制均没有触点上报，不能替代实际四方向边缘/握持测试。恢复后的常驻服务
  另观察到 670 次触点输出，但没有同步操作标注，不能据此判定手势质量或是否鬼触；
- 程序备份与前后 trace 比较位于 `deploy-logs/orientation-4xokQW45/`；
  正常录制：`deploy-logs/session-P5wcQvar/`；DPMS 录制：
  `deploy-logs/session-ST5ydEDS/`。

该次部署的 daemon SHA-256：
`4700aaa627d1fe666bd5880df485b67a922f82c996aa3a3d8249320d35b9a2b9`。
截至该次验证，真正的系统休眠、其他桌面方向来源、首条桥接消息的初值握手及有标注的
实际触控验收仍待完成；Android 未部署。

2026-10-23 启动方向缓存握手增量验证：

- 接收器移到设备打开和 STREAMON 前；初值请求只发一次且非阻塞。桥接用三秒内
  的缓存应答，KDE 查询移到独立后台线程，触控 daemon 默认仍为单线程；
- Release 与 ASan/UBSan 各 18 组通过，sanitizer 使用 `detect_leaks=0`。
  真 socket 测试覆盖请求字节、来源地址、真实凭据、缓存过期/清除、异常版本/截断、
  意外 fd 清理，以及慢查询期间缓存应答不等待查询。另以 root 运行 C++ socket
  测试和 Python 的 11 个测试，包含降权发送者的真实 UID 拒绝；
- 本机更新了 `himax-touchd` 与 `himax-screen-bridge`，模块未重载。桥接可用时
  的 873 帧录制从第一帧起均为 `rotation=3`，没有 `-1` 初值帧；bad/stale、
  dropped/recording_error 均为零，严格回放 `live_output_verified=1`；
- 停止桥接后的 552 帧录制均为 `rotation=-1`，仍能持续求解并严格回放一致，四项
  错误计数也为零。之后恢复桥接，常驻触控进程不重启即收敛到方向 3；
- 两个服务均 active、`NRestarts=0`，触控 daemon 为单线程。两次录制均无触点，
  只证明启动、方向获取/回退及录制链路，不能证明实际手势、掌压或四方向手感；
- 旧程序备份及安装哈希位于 `deploy-logs/direction-handshake-l2nIppN5/`；
  桥接可用录制：`deploy-logs/session-ByZ67TXY/`；
  桥接缺失录制：`deploy-logs/session-JVKyMpjV/`。

已安装 daemon SHA-256：
`ebb6afc6001cf9653c987b59c829b8e31913bd3076fa4b25ff43f68310272703`；
桥接 SHA-256：
`b0f281a606dc7fdc4ff5cab37106307ceb3c3a85a33a60a680ddedbd562ac17a`。
本次没有执行真正系统休眠或修改 Android。桥接冷启动/无新鲜缓存时仍可能先使用
上游默认方向；实际四方向触控、系统休眠与长期稳定性仍待单独验收。

2026-10-23 快速边缘滑入修复验证：

- Release 与 ASan/UBSan 各 23 组通过，sanitizer 使用 `detect_leaks=0`。
  96 条合成 raw 滑入覆盖四方向、四边、两种速度、轴向及正负斜向；
  另有屏内移动、物理首末行列/角落和修正拒绝条件测试；
- 32 条真实求解管线的合成 A/B 序列，首报帧、释放帧和 tracking ID 完全一致。
  方向 3 的代表性样本中，左/右边首报距边缘约 `231 → 0.2/0` 像素，
  上边 `99.1 → 0.1`、下边 `219.2 → 0`。这些是 `2560×1600` 面板坐标
  下的合成结果，不是用户本次实际动作的测量；
- 若第一份有效候选已超出原有 ER 最大范围，则整个轨迹与关闭修正的对照一致。
  不为无法证明来源的远端起点造边缘坐标；靠边按下后立即朝内滑动仍可能触发补偿；
- 新测试在修复前触发了上游 `Contour.cpp::HighResCache::Row` 的 heap-buffer-overflow，
  原因是最后一行/列的 cubic window 仍取到 `len`。现在仅将越界抽头限制到最近
  有效行/列，所有原本有效的取窗保持原值；对照与修复程序都包含这项内存修复；
- 本机已更新 `himax-touchd` / `himax-replay`，未重载模块。六秒闭环录制 672 帧，
  bad/stale/dropped/recording_error 全零，严格回放 `live_output_verified=1`。
  该样本无触点，只证明设备采集、求解、方向及录制链路；
- 程序及测试日志备份：`deploy-logs/edge-onset-0IB2FR2j/`；
  本机录制：`deploy-logs/session-GpLw6vqw/`。

本次安装的 daemon SHA-256：
`cc2e3269ee76e4e1ecb270eaf1662815af8ddf1962eb4f562714c9a5eaf33e25`。
实际快速滑入手感、靠边点击/游戏操作以及 Android 全面屏手势尚未验收；
Android 仍未迁移或部署此用户态架构。

同次构建另包含此前完成的 master 健康状态修改：完整 checksum body 全 `0xEC`
时按上游 BusFiller 立即请求 SOS；其他坏帧仍连续五帧才恢复，好帧/新 epoch 清除
累计状态；reset 失败时保持请求供后续重试。分类、计数饱和、最后完整 THP 缓冲
保留、录制/严格回放的恢复一致性已通过 host tests，但未对真实 SPI 注入故障。

2026-10-23 AFE 邮箱生产协议验证：

- 从硬件驱动提取 `hx-afe.c`，V4L2/legacy 共用同一份实现；事务语义保持为
  body16 → trigger4 → read16，失败不推进 slot，不把读回解释成校准完成；
- 全部 65,536 种命令/参数包与直接编译的固定版本 Windows 构包函数逐字节一致；
  五个槽位分别覆盖三步事务故障与同槽重试。测试不向设备发送这些遍历命令；
- `make check`、V4L2/legacy 模块构建通过；新增参考源集成检查后，Release 与
  ASan/UBSan 各 25 项测试通过，sanitizer 使用 `detect_leaks=0`；
- 实机 trial 的观察录制 515 帧、正式安装观察录制 514 帧，均无坏帧、过期帧或
  录制丢帧；正常求解观察到清除 calibration-done 和进入 IDLE。watchdog 与带
  MMAP 的设备解绑退出测试通过。此处没有真实手指样本，不代表手势已验收；
- trial 及安装备份：`deploy-logs/v4l2-20261023-032904/`、
  `deploy-logs/v4l2-20261023-033014/`。仓库产物、DKMS 缓存、磁盘与运行中模块
  build ID 均为 `010f2441b66cab67c6547d2ee7c3c9b253295ccf`，模块 SHA-256 为
  `92702b4c59817bd106357fcf10af9f426a397d21a5090f5fae7fc65ffb45e305`。

这组测试未覆盖真实 SPI 事务故障注入、所有动作的固件完成握手、物理充电切换或
真实系统 suspend/resume。算法和参数未改，Android 未部署。

同日真实固件完成握手补充（独立于上述 25 项 host tests）：

- `scripts/test-v4l2-afe.sh --basic` 通过，证据在 `deploy-logs/afe-7FIbU1vL/`：
  先观察 24 个无手指帧；`ClearStatus(8)` 后 `word[3]: 0xbb → 0`，
  `StartCalibration(0)` 后 `0 → 0xbb`，再清除后回到 0。`EnterIdle(0)` 后约
  3.06 秒收到 `word[0]=1`。38 帧原始录制全部校验通过；扫描率仅计传输验证；
- 完整测试两次在跳频步骤失败，原始证据分别为 `deploy-logs/afe-b1gkGwrJ/`、
  `deploy-logs/afe-qcq3EloU/`。两次都在 sequence 38 成功传输 `ForceToFreqPoint(1)`，
  到 sequence 638 仍为 `word[10]=0, word[2]=0`；各 638 帧均通过 checksum。
  不是 SPI 读帧停滞，也不能把 ioctl 成功算成频点 1 生效；
- 当前上游 `TsaCoreSolver::InitCore()` 不填写 `freqHopMode`，默认值为 0；
  `Afe::GetNextScanFreqIndex()` 只有模式 1 才请求手动跳频。Linux 保留该默认值，
  没有因此启用新的手指跳频策略。原厂反编译 `thp_afe_force_to_freq_point`
  也使用 `0x0c, idx`，且从 suffix word10 选频率表；命令未见完成的原因仍待定位，
  可能涉及初始化/固件模式，但目前没有证据足以据此修改生产寄存器或 suffix 映射；
- `scripts/test-v4l2-transport.sh` 在新模块上通过，跨 epoch 的第二次 reset 于
  381 ms 被正确拒绝；记录在 `deploy-logs/transport-RTZkp9sV/`；
- 所有实机检查退出后都恢复常驻服务。完整测试仍如实返回非零；`--basic` 是用于
  单独检查其余已完成握手的选择，不表示已完成全部 AFE 验收。

后续应优先定位“命令被接受但频点/完成位不变”：核对当前固件支持与原厂自动/手动
跳频模式、初始化前后寄存器状态，拿同样命令序列的 Windows raw 作对照；不能仅凭
上游函数存在就宣称已在两端验证，也不应盲目向默认路径追加关闭自动跳频的命令。

同日邮箱读回及校准时序隔离实验：

- 临时诊断模块完整测试：`deploy-logs/v4l2-20261023-034913/`，仍在频点 1 完成
  检查失败。读回为 `a8 8a 0c 00 01 00 00 00 00 00 00 00 00 00 4b 75`，
  确认命令、参数及 checksum 字节确实写入预期槽位；这仍不是固件完成证明；
- 不再显式校准的隔离实验：`deploy-logs/v4l2-20261023-035122/`。清除启动校准标记后
  等待 240 个无手指帧（2,000 ms），sequence 272 发送切频，到 sequence 872 仍无
  频点/完成位变化。现有读回里该命令头已变为零，参数 1 和 checksum 保留，符合
  固件消费邮箱的行为，但不能据此认定切频完成。872 个试验帧均通过 checksum；
  此前独立 smoke 中有 3 个启动 sync 错误，随后 512 帧正常，未掩盖为全零错误；
- `basic` 临时模块试验：`deploy-logs/v4l2-20261023-035404/`，显式校准、清状态、IDLE
  通过；关掉邮箱日志后的正常求解、watchdog 和带 MMAP 解绑同样通过；
- 三次试验均恢复之前安装的普通模块和常驻服务，没有持久化诊断模块，也未改算法、
  baseline、参数或 Android。普通构建检查确认不含邮箱日志格式串和模块参数。

这排除了“参数 1 没写入邮箱”和本次“紧接显式校准过快发命令”两个解释，但没有证明
Windows 上同一命令会生效。应将其标为本机手动跳频能力未验收，而不是已证实的
Linux 专属回归。当前不因此更改 `freqHopMode`、扩展未验证的固件命令或启用笔路径。

2026-10-23 RawOut 启动顺序与部署交接验证：

- 启动修复 `cd772a9`、部署脚本修复 `f1bcef3`。`make check` 通过，Release 26 项通过，
  新增 raw-power-on 故障测试的 ASan/UBSan 通过；legacy + 邮箱诊断构建也通过。
  这不是实机 SPI 故障注入，也未执行系统休眠；
- 首次临时模块检查 `deploy-logs/v4l2-20261023-042055/`：514 个有效 smoke 帧，
  bad/stale/dropped/recording_error 全零，solver/watchdog/MMAP 解绑通过。
  首次正式安装 `.../v4l2-20261023-042158/` 则有两个启动 sync 错误，随后 515 帧有效；
- 这两轮部署前旧常驻服务在 `mask --now` 期间被 watchdog 终止，journal 显示通知被
  拒收，core 堆栈在 poll。没有把这两次异常算成正常停止。修改顺序后，
  `.../v4l2-20261023-042802/` 临时试验记录 `previous_service_stop=success`，
  516 帧中仅首个 all-zero 帧被启动容错接纳，后续 515 帧有效；全套临时检查通过；
- 新脚本正式安装 `deploy-logs/v4l2-20261023-043011/`：旧服务正常停止，515 个
  smoke 帧全有效；DKMS/dracut 期间临时求解服务保持 active 且只有一个触摸 input。
  临时服务从 04:30:32 到 04:32:39 共处理 911 帧，bad/stale 为零（进入 IDLE 后
  帧流稀疏），最后正常停止并移交给常驻服务；后者和方向桥接均 active，
  `NRestarts=0`。临时服务 `touch_reports=0`，不能据此证明真实触控手感；
- 仓库、磁盘和 DKMS 缓存模块 SHA-256 均为
  `eabb60168f3e35310dffa4e8d4eb2b843ac6d59412823b96026ecbffea1135bd`，
  运行中 build ID 核对为 `5ca58ad4ba07b5075c4abf7bc04385c78b1fd729`。
  模块未编译邮箱诊断，daemon 算法二进制未变化；新版离线 replay 已随安装更新。

本轮不声称修复快速滑入首点回归、手动切频或掌压亮屏问题；没有部署 Android。

2026-10-23 快速滑入超出窄带的增量修复与部署：

- 峰检测前保存八帧固定大小的边界信号摘要。首次候选超过 ER 窄带时，仅在
  安静帧 → 单处局部贴边脉冲 → 内移候选的连续时序、位置和速度均一致时补回
  首报边缘坐标；不改变检测/准入阈值、baseline、消抖或 history-replay 标志；
- 先前遗漏的 `rotation=3 / edge=2 / speed=3 / drift=0` 合成样本从
  `y=218.3` 修正到 `y=0.1`，均在同一帧首次上报。Release 与 ASan/UBSan
  各 27 项全部通过，其中 240 组完整求解 A/B 的准入/释放帧和 ID 一致，
  12 组没有边界前置信号的屏内起始轨迹完整保持上游行为；证据单测覆盖
  一种接受和 19 种拒绝/生命周期情况。`git diff --check` 通过；
- 仅安装 Release 的 `himax-touchd` / `himax-replay` 并正常重启服务，没有重新编译
  或替换内核模块，工作树中尚未完成的 AFE 初始化改动没有部署。
  旧程序、源码差异、安装哈希及运行检查在 `deploy-logs/edge-entry-uN6MHmcC/`；
- 部署后首段检查：664 帧，bad/stale 均为零，求解耗时 mean=135 µs / max=1019 µs，
  常驻服务 active、`NRestarts=0`，屏幕方向为 3。该窗口没有触点，耗时不能用于推断
  滑动负载或改动前后性能差异，也不能替代真实边缘手势验收；
- 已安装 daemon SHA-256：
  `b190856fc0d66f4a36c4ada01828eb32af8bab607559f7efa937f5a17eed8794`；
  replay：
  `4ea59585674ef0682b043f924d7c6ae59d225a16f9294aa03152e2887e664ed3`。
  磁盘模块仍为上述 `eabb6016…`。本轮未部署 Android。

2026-10-23 inspection/N-frame 准备移植与实机验证：

- 实现和直接源码参考在 `6a819ad`；`make check` 通过，包括 legacy 算法 host
  回归、普通 V4L2 模块和 29 项 Release CTest。新增 raw-scan-preparation、
  原有 raw-power-on 及源码提取集成测试的 sanitizer 构建通过；另行编译 legacy
  与 V4L2 邮箱诊断模式均通过。这不表示重新运行了全部 29 项 sanitizer 测试；
- 临时普通模块：`deploy-logs/v4l2-20261023-050318/`，513 帧全部有效，
  bad/stale/dropped/recording_error 均为零；solver/watchdog/MMAP 解绑测试通过，
  随后恢复原模块；
- 正式安装：`deploy-logs/v4l2-20261023-050426/`，smoke 共 513 帧，其中首个
  sync 错误由既有启动容错接纳，其后 512 帧有效。临时 solver 的另一次 AFE
  启动也有一个 sync 错误，至正常交接共 909 帧，之后未新增错误，stale=0。
  DKMS/dracut 期间服务保持 active 且只有一个触摸 input，交接正常；
- 仓库产物、磁盘模块、DKMS 缓存 SHA-256 一致：
  `08601d1e3e76bab95c75da4ad78bb14a408b4b8afc43594c119f91e5d162641f`。
  运行 build ID 与磁盘核对一致：
  `93c947280180af45512928e6d2d797a6cda2e2ff`。普通模块未启用邮箱诊断；
  daemon 保持上一项边缘修复的 `b190856f…`，本轮没有修改求解算法；
- 新初始化下完整 AFE 测试仍在切频步骤失败：
  `deploy-logs/afe-A3sqeYoQ/`。清状态、显式校准及 ack 均成功；sequence 38 的
  frequency=1 命令传输成功，到 sequence 637、五秒期限仍为 freq_slot=0 /
  freq_done=0。637 个原始帧全部校验通过。完整测试如实返回 1，没有把传输成功
  认作切频完成；
- 单独 `--basic`：`deploy-logs/afe-lGFIQZB2/`，显式校准、清状态、IDLE 后续
  suffix 均确认，38 帧有效，startup_invalid=0。此次 IDLE 标志在命令后约 11 ms
  出现；没有受控 A/B 证据证明相比旧记录的时序差异来自本次改动。扫描率仍只有
  传输证据。两项测试结束均恢复常驻服务，服务与方向桥接 active、NRestarts=0。

这些窗口没有真实触点，不证明快速手势、真实 suspend/resume、掌压亮屏或长期负载
验收通过；也没有部署 Android。前置准备的事务对照已完成，完整硬件生命周期仍需
继续核对，尤其是初次 reset 前的 reload 清零、Linux IC 检测的额外 safe-mode 动作、
raw 类型选择的验证/重试，以及后置 AP/USB/sorting/N-frame 重放与 Windows 的差别。

2026-10-23 power-on/raw 类型对齐（`c496521`）与本机部署：

- 默认入口改用 `hx_raw_start_scan()`，执行准备、Windows power-on、完整 reload
  字轮询、50 次 raw 类型验证和只写 producer handshake；移除 default 的 A8 门槛、
  power-on sorting 读回及后置 legacy runtime replay。旧入口行为保持不变；
- `raw-master-startup` 用上游八个未改函数体及实际 Init master 调用片段对照完整
  寄存器事务、回调和延时。覆盖每笔正常事务的传输故障、所有 reload 次数边界、
  四个辅助状态读取失败、raw 验证的 0～50 次比较/读/写故障和最后一次错误优先级；
- `make check` 通过：checkpatch、全部 host 回归、普通模块及 30 项 Release CTest。
  新启动、原准备、legacy power-on 和源码提取集成四项 ASan/UBSan 测试通过。
  legacy、V4L2 邮箱诊断、普通 V4L2 三种构建通过；编译器/pahole 版本差异警告仍在，
  没有构建错误。未重新运行完整 30 项 sanitizer；
- 临时普通模块 `deploy-logs/v4l2-20261023-052211/`：513 个 smoke 帧，首个全零帧
  由既有启动容错接纳，后续 512 帧有效。solver 共 874 帧、bad/stale=0；
  watchdog 与 MMAP 解绑检查通过，并正常恢复旧模块；
- 正式安装 `deploy-logs/v4l2-20261023-052321/`：515 个 smoke 帧，首个全零，
  后续 514 帧有效，录制无丢帧/错误；DKMS/dracut 期间临时 solver 共 912 帧、
  bad/stale=0，安装及服务交接正常。仓库、磁盘、DKMS 缓存 SHA-256 一致：
  `bd2c9cbe28692c9b3eaaf843e420afecf2a47cc455a9c8d7e4cb8d3ce5ec1392`，
  运行 build ID 与磁盘一致：`9653f551c0c1eab4b659e352da87ec03ec22c418`；
- 安装后 basic AFE 检查 `deploy-logs/afe-TNy7g1zC/`：显式 calibration-done、
  clear-status/ack、IDLE 后续 suffix 全部确认，38 帧有效、startup_invalid=0。
  此次 IDLE 在约 3061 ms 后出现，不能将不同记录的时序差异当作性能改善。
  扫描率只有传输证据；本次未重测手动切频，上一轮的未完成结论仍保留；
- 检查后常驻服务与方向桥接 active、NRestarts=0；常驻 solver 877 帧、
  bad/stale=0。普通模块邮箱诊断未编译，daemon 二进制沿用边缘修复版本。

这些实机检查无真实触点，不能据此宣称手势、掌压亮屏、真实系统睡眠或长期触控性能
已经验收；Android 未修改。接下来先对照 AHB 原语的 `0x12`/`0x10` 与增址模式差异，
再移植 initial reset/IC detect/sense-on 的剩余行为。

2026-10-23 AHB 协议移植（`33b01f5`）：

- 默认寄存器事务接入共享 `hx-ahb.c`，对齐 `0x12/0x13` burst、无反向切换、
  256 字节读限制和 interface-on 的十次轮询语义；legacy 与 OTP 专用序列保持不变。
  `make check`（checkpatch、全部 host、普通模块、32 项 Release CTest）通过；
  AHB 协议及源码提取集成两项 ASan/UBSan 通过；legacy、邮箱诊断 V4L2 和普通
  V4L2 三种模块配置均编译成功。未重新执行完整 32 项 sanitizer；
- 带邮箱诊断的 full AFE 试载 `deploy-logs/v4l2-20261023-053704/`：smoke
  517 帧，首个全零、其后 516 帧有效；校准、清状态与 ack 均成功。切频 1 在
  sequence 38 传输成功，五秒后 sequence 638 仍为 freq_slot=0 / freq_done=0，
  测试返回 1 并恢复旧模块。638 个 AFE raw 帧全部校验通过。此次协议对齐未解决
  手动切频未完成的问题，也没有足够证据将其归为 Linux 独有故障；
- 普通模块试载 `deploy-logs/v4l2-20261023-053804/`：514 个 smoke 帧，首个全零
  由既有启动容错接纳，其后 513 帧有效，stale/dropped/recording_error=0。
  solver 至停止共 873 帧、bad/stale=0、touch_reports=0；watchdog 与持有 MMAP
  时解绑设备检查通过，结束恢复旧模块。
- 正式安装普通模块 `deploy-logs/v4l2-20261023-053908/`：517 个 smoke 帧，首个
  全零、后续 516 帧有效，stale/dropped/recording_error=0；临时 solver 在
  DKMS/dracut 期间保持运行，至交接共 913 帧、bad/stale=0、touch_reports=0。
  仓库产物、磁盘和 DKMS 缓存 SHA-256 均为
  `55514d534aac55dd376274d9e9623cd5b42874ef2da95983c0cf3d1d2842adb7`，
  运行 build ID 与磁盘一致：`cb0207f211b27fa3308407f625dc886aff9f6389`；
- 安装后 ordinary basic AFE 检查 `deploy-logs/afe-RIWk28Qf/`：校准、完成标志
  清除/ack、IDLE suffix 均确认，39 个 raw 帧全部有效、startup_invalid=0。
  IDLE 此次耗时约 3057 ms；扫描率仍仅有传输证据，不能推断性能提升；
- 普通模块不含邮箱诊断参数；daemon 二进制和边缘补偿算法没有变化。常驻服务与
  方向桥接均恢复运行。所有上述窗口无真实触点，不覆盖实机边缘/多指/睡眠手势、
  掌压亮屏或长期负载验收，Android 未修改。

下一阶段核对 initial reset 前的 reload 清零、IC detect 的额外安全模式和 native
sense-on，再核对 SPI 封包、实际传输长度与错误重试。上游未启用的功能不能自行打开，
手动切频仍需 Windows 同命令实机对照或新的固件状态证据。

2026-10-23 reset/sense-on 移植（`f27e70e`）：

- 初始与 OTP 后的 reset 现通过共享 `hx_ahb_hw_reset()` 清 reload、切 GPIO、
  开 burst，调用方等 10 ms；IC ID 只读校验保留，不再为此进入旧 safe mode。
  sense-on 调用共享实现，接口唤醒 → ISR 清零 → 11 ms → GPIO 低/高。
  GPIO 仍保留 native 20/50 ms 时序；probe 改为先释放 RESET，避免首笔 SPI
  写发生在 reset asserted 状态。OTP 自身的安全模式与 legacy 路径保持不变；
- AHB 源码 oracle 增加未经修改的 Windows `hx_hw_reset_ahb_intf` 和 `hx_sense_on`，
  对照普通/延迟 interface-on 下所有总线和 GPIO 故障的停止位置、11 ms 等待及
  两种 sense-on 分支。`make check`（32 项 Release）通过；相关两项 sanitizer
  通过；legacy、普通 V4L2、邮箱诊断 V4L2 均编译成功。未重跑完整 sanitizer；
- full AFE 诊断试载 `deploy-logs/v4l2-20261023-054919/`：OTP 仍为 W273AS2700，
  slot=0x0f18、revision=0x46、E2=0、valid=3。smoke 共 516 帧，首个全零、
  后续 515 帧有效。校准和清状态成功；切频仍在 sequence 38 发送后、五秒期限
  sequence 638 未完成，返回 1 并恢复旧模块；638 个 AFE raw 帧全部有效。
  不再将重复同一失败测试当作进展；除非出现新协议/固件证据，不继续重复切频；
- 普通模块试载 `deploy-logs/v4l2-20261023-055026/`：smoke 共 514 帧，首个全零，
  后续 513 帧有效、stale/dropped/recording_error=0；solver、watchdog、MMAP
  解绑检查通过，结束恢复旧模块。启动日志中不再出现旧 `himax_sense_off`，但不代表
  OTP 的专用 safe-mode 序列被移除，也不代表真实系统睡眠已经验收。
- 正式安装完成：`deploy-logs/v4l2-20261023-055132/`。仓库产物和已安装模块
  SHA-256 均为 `8b48c3c983effa074d555155489e9270fa1094617a2497bacf56ce161dd1a00a`，
  部署脚本核对运行 build ID 为 `d9175dce3a0b1ca67573640cd517ed61d6611ff1`。
  临时 solver 至交接累计 911 帧、bad=1、stale=0，不能将其描述为全程零错误；
  `touch_reports=0`，不构成实际手势验证。交接后常驻服务与方向桥接均 active，
  `NRestarts=0`；已安装 daemon 与仓库 Release 产物一致，仍包含上述边缘首报修正。
- 安装完成后重新运行四方向 `solver-edge`、完整 A/B `edge-onset-reference`
  和 `edge-onset-evidence`，六项均通过；`git diff --check` 通过。
  尚未验证真实快速入屏手势，Android 未部署该用户态路径。

2026-10-23 native SPI 对照与 GENI 控制器前置修复：

- 默认读取最多十次、取消/停止错误终止；完整长度通过后才复制读结果，短成功不
  重试。写入保持单次并拒绝短成功；读写的极小容量和 u32 溢出边界先验证再计算。
  直接源码测试比较 native SPI 与固定 Windows bus 方法，明确区分 native 保护；
- `make check`（34 项 Release）通过；完整 34 项 ASan/UBSan 通过；随后新增的
  `geni-completion-policy` 使用实际补丁并单独在 ASan/UBSan 下通过，35 项 Release
  全部通过。legacy 诊断、邮箱诊断 V4L2、普通 V4L2 三种模块均编译成功；
- 第一次普通试载 `deploy-logs/v4l2-20261023-060313/` 失败并回退：第一笔写入
  `Short SPI write: 0/3`。核对本机 GENI GSI callback 与 SPI core 后确认提前设置
  message success 会跳过长度累计。没有将这次试验改记为通过；
- 控制器补丁在 `kernel-patches/`，未改其他内核/buildbot 仓库或已安装 headers。
  用匹配 headers 独立编译修正的控制器模块，先核对本机只有一个 SPI 控制器和
  一个 Himax 从设备，再进行临时替换；记录 `deploy-logs/geni-completion-U5LrqQG6/`；
- 依赖补丁的普通试载 `deploy-logs/v4l2-20261023-060921/` **安全通过结论撤回**：
  脚本返回 0，515 个 smoke 帧，
  首个全零由既有启动容错接纳，其后 514 帧有效，stale/dropped/recording_error=0。
  solver/watchdog/MMAP 检查返回成功；但包裹整个控制器试验的
  `deploy-logs/geni-completion-U5LrqQG6/kernel.log` 已记录两次内核 Oops，第一次甚至
  发生在 smoke 前。脚本未检查内核故障而误报通过；恢复模块不能修复内存破坏。
  用户随后报告两次强制重启。Android 未修改。

2026-10-23 GPI 生命周期事故调查及离线修复：

- 首次 Oops 在同一 boot 的 monotonic 52832.535 秒，加载实验控制器命令在
  52830.306 秒；第二次 Oops 在 52883.692 秒，回退原控制器命令在 52882.824 秒。
  首次 DRM、后续 memcg 坏指针基值相同，且与 SPI 配置前八字节完全一致；
- 本机 GPI 源码及已安装模块反汇编确认释放 `config` 后未清空，重新配置直接
  `krealloc` 旧指针；离线提取原函数，在 ASan 下稳定复现 double-free。
  host harness 额外清空指针的对照通过，没有加载任何实验驱动；
- 新增宿主补丁 0002 与固定原函数 fixture 的生命周期回归，只修复配置所有权。
  不改固件、用户态算法或实际运行内核；尚未完成修复内核的启动/卸载验证；
- 14:41 启动后核对当前加载原控制器 build ID
  `b0f0627e8598dfb054a697204ece81cb84e8a115`，触摸模块仍为 `f27e70e`。
  EFI initrd 中也为原控制器；遗留 `updates/spi-geni-qcom.ko` SHA-256
  `ecf3c89c…` 已移到 `deploy-logs/gpi-lifetime-C4K1zALV/quarantine/`，
  并重建 depmod 索引，磁盘解析恢复原模块。此次未重载任何运行模块；
- 新增部署内核健康门控：采集前建立 journal cursor，拒绝已有 Oops/坏页内核，
  检查阶段及结束时保留新日志与 taint；新故障、重启或证据读取失败均不能报告成功，
  故障时停止进一步 live rollback/reload。此检查不能证明没有静默内存破坏。
- 离线验证：`make check` 通过（37 项 Release CTest、legacy 回归和普通 V4L2
  模块构建）。GPI 生命周期与 GENI completion 测试自身使用 ASan/UBSan；
  本轮没有重跑完整 37 项 sanitizer 套件。
- 在独立临时目录对本机完整 GPI 源码应用 0002，使用匹配 headers 构建 `gpi.ko`
  成功，仅有编译器/pahole 版本差异警告。产物未安装、未加载，原源码未修改。
  部署健康 helper 也在本机 journal 上完成只读验证，未操作硬件。
- 最终成功信息延后至 EXIT 健康检查结束；试载还必须完成旧模块恢复及恢复后的
  健康检查。回归覆盖 20 个健康门控场景和 6 个退出/恢复结果场景。

2026-10-23 宿主前置修复完整内核构建：

- 在编译服务器的独立目录重建与本机 Image/config 对应的源码基线，断言 Git tree
  一致后加入 GENI completion 与 GPI 配置生命周期两项补丁；原基线未修改；
- 新内核 `7.1.8-gaokun3-himax1` 的 Image、模块、设备树，以及触摸仓库 `a06e05c`
  的普通 V4L2 模块均构建成功。原配置只调整版本后缀、开启 uinput、关闭旧树内触摸；
- 最终归档中的模块架构/vermagic、依赖和修复函数反汇编均已核对，产物取回至
  `artifacts/host-kernel-a06e05c/`，本机 SHA-256 清单校验通过；
- 尚未安装、生成 initramfs 或启动验证，当前运行模块没有替换。
  [宿主补丁记录](../kernel-patches/README.md) 保存基线、产物哈希和后续验收边界。

2026-10-23 独立测试内核安装：

- 新增 `Fedora Linux 44 (Himax SPI/GPI test)` 启动项，版本
  `7.1.8-gaokun3-himax1`。已安装匹配模块并生成 initramfs，解包后的
  GPI/SPI/Himax/uinput 与安装产物逐字节一致；
- 为避免旧 DKMS 源码在测试启动时覆盖模块，仅新启动项追加
  `systemd.mask=dkms.service`。当前测试包尚未纳入 RPM/DKMS 管理；
- 原有 167 个 ESP 文件、旧内核和旧模块的哈希复核通过，默认启动项不变。
  没有重载模块或重启；当前仍运行原内核，真实启动/触控验收待用户切入新项后进行；
- 证据位于 `deploy-logs/host-kernel-install-himax1/`，详细状态见
  [宿主补丁记录](../kernel-patches/README.md)。

2026-10-23 新内核启动与 master 整帧读取对齐：

- 用户已重启到 `7.1.8-gaokun3-himax1`；GPI/SPI/Himax 运行 build ID 与构建匹配。
  短期帧流增长、服务零重启、bad=2 未增长，本次未发现内核 Oops/坏页/Himax SPI
  错误。详见宿主补丁记录，不把成功启动等同于长期稳定或睡眠唤醒验收；
- 复核固定 Windows `FrameLayout.h`、`HimaxChipFrame.cpp`：master 为 5063
  线上字节，扣除三个 SPI 前缀为 5060。旧 native 读取按 5132 容量分为
  5129+3，产生额外 event-stack 命令。现在一次读取完整 master，成功后将
  HXST 尾部 72 字节清零，失败时保留完整上一帧。probe 拒绝不能承载完整
  5063 字节事务的控制器，不再隐式拆帧；legacy 内核算法读取路径未改；
- ABI v2 的结构大小保持不变，INFO/每帧 payload_size 改为 5060；新 daemon、
  回放、AFE 测试按有效长度解析，同时支持旧 5132 格式。旧 daemon 不兼容新
  模块，须先更新用户态。padding 不冒充硬件采样，5132 旧记录尾部仍按证据保留；
- 新 `native-event-read` 直接提取生产读帧函数和 SPI 原语，覆盖线上命令、
  完整 raw/checksum/suffix、传输边界、短读、重试、停止错误和上一帧保护。
  录制/严格 solver 回放测试覆盖两种长度、非法长度和 padding，以及跨 epoch
  的新旧格式会话。重启前 `make check` 通过；本轮重跑 38 项 Release 全过。
  ASan/UBSan 的 38 项验证跨重启完成，删除无用测试宏后另重跑上述三项通过；
- 在编译服务器保留原内核输出，使用独立 `master-read-HtPArJ` 模块目录构建
  当前候选，vermagic=`7.1.8-gaokun3-himax1`。源码内容已与本地逐文件校验，
  构建无 warning/error，取回 `artifacts/master-read-himax1/`。
  模块 SHA-256：`cd117f996251e4bb2a76a424b551aaf2043d131f5c25ed95f2348feb6a93680f`，
  build ID：`8373e1e315a0325f115c53592e07a28afd51c94b`；
- **该单次 master 读取候选尚未部署**，当前运行仍为 `a06e05c` 触摸模块。
  历史 1027 个有效采样的尾部均为零只是辅助证据，不证明新物理事务已正确运行。
  下一步仍需实际采集/手势、IRQ/清帧语义与切频完成审计；Android 新架构未部署。

2026-10-23 `44e6917` 单次 master 读取正式安装到测试内核：

- 按用户要求替换当前 `7.1.8-gaokun3-himax1` 的外置触摸模块及配套 daemon/replay，
  未热替换 SPI/GPI、未更改 kernel Image/DTB、未改 Android 或其他版本模块。
  该内核没有本机 headers，使用上述服务器构建产物，不调用 DKMS；启动项的
  DKMS runtime mask 保留。原模块、程序与 initramfs 均备份到
  `deploy-logs/master-read-install-44e6917/`；
- 安装前重新运行 38 项 Release CTest 全过。五秒 smoke 514 帧，首帧全零由
  启动容错丢弃，后续 513 帧校验通过，recording_error/dropped/stale=0。
  文件首记录 payload_size=5060。临时 solver 876 帧、bad/stale=0；无触摸输入，
  不构成手势测试。交接后的常驻服务 1798 帧、bad/stale=0、touch_reports=443、
  NRestarts=0；真实触控报告存在，但未验证用户感知的平滑度/边缘手势；
- 运行与磁盘模块 build ID 均为 `8373e1e315a0325f115c53592e07a28afd51c94b`，
  SHA-256 与上一节候选一致。已安装 daemon 与 Release 产物逐字节一致，SHA-256
  为 `764ac6045bdefc8d42dbbebc2f441d35fb1488aaa72d0295b40e98e33d13096c`；
- 重建 `/boot` 与 ESP 同一测试启动项的 initramfs，解包核对 Himax/GPI/SPI/uinput
  与磁盘模块逐字节一致，kernel Image/DTB/BLS entry 哈希未变。新 initramfs
  SHA-256：`88d55c7559007d53ef23171b187e61a7f78175d671ac1ca4b97a1a7b7f58b17f`；
- 部署、交接、更新启动镜像后的健康门控均通过，没有新 Oops/WARN/坏页。
  没有自动重启，更新后的 initramfs 仍待下次开机验证；默认启动项保持原旧内核，
  要重启试用本版本仍选 **Fedora Linux 44 (Himax SPI/GPI test)**。

2026-10-23 取帧有效性对照（代码已修改，未更新运行中的 daemon）：

- 直接读取固定 Windows `HimaxChipFrame.cpp` 的 `CheckFrame` 和
  `HimaxAfeAdapter::ValidateMasterChecksum`：两者都只对 THP `[5, 5063)` 的
  小端 16 位字求和，要求总和非零且低 16 位为零；不要求 THP 字节 3/4 固定。
  Linux 原来额外要求 native `[0, 2)` 为 `5a a5`，已经移除该非等价拦截；
- 新 `frame-check-reference` 编译经完整 SHA-256 校验的 Windows 函数原文，
  对比全部 65536 种头字、5058 个 body 字节的单 bit 损坏、全零/填充、512 组
  随机网格及修复后 checksum、旧记录尾部。先在旧实现得到分类不一致失败，再
  修正实现。提取测试另检查完整函数/常量、源漂移/缺失拒绝以及稳定生成；
- 历史六份 HXST 共 3214 帧中，6 帧非固定头也都没有合法 checksum，因此新旧
  接受/拒绝集合相同；**没有证据证明这次修改能解释或修复历史断触**。最新部署
  smoke 的 514 帧严格 session 回放通过，513 帧 solver 输出/动作验证一致；
- 同轮 IRQ/恢复审计确认：Windows `IntOpen/GetFrame` 操作 THP 中断通道/缓存；
  Linux 直接 threaded IRQ 读硬件，启动时三次间隔读不等价于消费 THP 旧队列。
  Windows 活跃/IDLE 的 200 ms 读取、重开次数与预算，也不等价于 Linux 四秒
  producer timeout。不能在 Linux 直接照搬 `IntOpen` 为无条件 IRQ disable/enable；
- Linux 异步初始化失败会调用 `vb2_queue_error`，daemon 的 POLLERR 路径退出，
  由 supervisor 重启；不是单凭 running=false 就永久卡在等待。普通错误的
  五帧恢复是本地 finger-only 策略，Windows 另有双通道不同计数和汇合条件，
  校验函数等价不代表整个故障恢复状态机已等价。后续仍需系统睡眠/失败注入验证。
- 验证：40 项 Release CTest 全通过；native/四面板、新 oracle/提取门控、录制
  与会话相关共 8 项 ASan/UBSan 通过，本轮没有重跑完整 sanitizer 套件。
  无内核代码变更，不需要新的模块构建。该用户态修正未安装，当前仍运行
  `44e6917` 的模块与 daemon，避免改变用户正在验证的版本。

2026-10-23 AFE 动作分发边界审计：

- 原 `afe-protocol` 证明邮箱包体与事务正确，但不覆盖从 solver action 到固件
  command 的映射。新增 `afe-dispatch-reference`：编译未经改写的生产
  `himax_afe_command()` 和经 SHA-256 固定的 Windows `AfeController` 方法；
  前者直接使用实际 `HardwareAction()`，后者使用原方法选择的命令号/参数。
  两端 register callback 均接生产 `hx_afe_send()`；这是分发层对照，不是独立
  Windows send_command/THP IOCTL/物理 SPI 的完整模拟；包体 oracle 仍由原测试提供；
- ClearStatus→0x06、ForceToFreqPoint→0x0c、ForceToScanRate→0x0e、
  EnterIdle→0x0a、StartCalibration→0x01 一致。遍历五个动作、256 个参数、
  五个槽位、三步事务失败及成功，确认支持参数的命令字节、顺序、slot 提交一致，
  native 保留 errno；Windows 只有成功 EnterIdle 后才 arm idle retry；
- native 接口有意比 Windows 公共方法窄：仅 ClearStatus(4/8)、frequency(0/1)、
  rate/idle/calibration(0)，无效/宽参数、reset 或未知 action 不产生邮箱 I/O。
  固定上游 InitCore 的 caps 只有一档扫描率，当前启用算法不需要其他 rate 索引。
  Windows 连接状态检查与 Linux queue owner/epoch/电源门控不属于本测试的同一
  层，测试并未声称已验证完整 ioctl 权限/生命周期；
- 提取门控覆盖原文完整性、Windows 漂移/缺失拒绝、native 变更自动刷新和稳定
  生成。42 项 Release CTest 全过，AFE 相关 4 项 ASan/UBSan 全过；本轮没有修改
  生产驱动或算法，也没有安装程序、重载模块、发送实机校准/切频命令；
- 没有发现足以解释手动跳频未完成的分发错误。frequency_done=0 不能单独证明
  当前运行故障，因为默认算法没有请求手动跳频；之前显式测试的传输成功但无
  suffix 完成仍是未验收项，不能改记为通过。需要 Windows 相同命令/固件状态
  的实际对照或新的协议证据，不重复同样失败试验；
- 部署版服务 17:56:52 CST 累计 5256 帧、touch_reports=1409、bad/stale=0，
  solver 平均 219 µs，仍运行此前 `44e6917` 程序。短期正常不替代睡眠、切频或
  真实手势质量验收。

2026-10-23 可选运行中采集实现：

- 新增 `LiveCapture`：管理线程接收有界 SPSC 事件流，在线程内创建独立私有目录、
  打开 `SessionRecorder`、排空并关闭窗口。触摸线程只提交事件/非阻塞唤醒；
  窗口关闭不能与新窗口交叉。未配置时不创建该管理线程/队列；配置但未触发时
  不逐帧复制证据。SIGUSR2 只置请求标志，不在 signal handler 中进行 I/O；
- 不调用 AFE reset/calibration、停止服务或重新申请队列。窗口 raw 仍是 HXRC，
  侧车显式区分 live-no-checkpoint 与 fresh；保留实际输出 digest、状态/方向、
  AFE 动作及结果，但不假装已序列化 baseline/TSACore 内部状态。严格回放拒绝
  live 文件，普通 raw 回放仅是从新状态重新求解；
- 目录错误、真实文件大小限制引发的后台写入失败、无帧窗口、非法事件和进程退出
  均有错误路径。上游队列丢失计数会传入 recorder 的 END footer，不会仅在日志
  中报告掉帧、文件却标零丢失成功。正常截止、提前结束和独立多次抓取均有测试；
- 新辅助脚本对运行进程读取参数，未配置/停止/检查期间更换 PID 时不发信号；
  本机原服务上实际执行该拒绝路径，PID 112080、NRestarts=0、已安装 daemon
  SHA-256 保持不变，没有发送 SIGUSR2。新版可选 systemd drop-in 只安装为文档
  示例，不默认启用；已在独立 DESTDIR 中模拟安装，并通过 systemd unit/drop-in
  静态验证，staging 为 `artifacts/live-capture-stage-gFVd9qX5/`；
- 尚未安装该用户态功能、未进行当前硬件在线录制试验，也没有验证慢盘长时间阻塞、
  真实 120 Hz 队列溢出、休眠期间的抓取或活动录制的时延影响。没有启用 TSan。
  完整 checkpoint、故障前预录、Android 集成仍未实现，切频未验收项不变。
- 最终代码的 44 项 Release CTest 和完整 44 项 ASan/UBSan 全部通过，新增的
  live/CLI 与既有 fresh 会话测试同时通过；另在设置测试超时后重跑相关测试通过。
  `bash -n`、`git diff --check` 通过。内核代码和 UAPI 未改，没有新的模块构建
  或 initramfs 更新；当前安装/运行版本保持 `44e6917`。

2026-10-23 `c00844e` 用户态部署与运行窗口首轮实机验证：

- 仅替换 daemon/replay，模块仍为 `44e6917`，没有卸载 Himax、GPI 或 SPI，也未改
  kernel Image/initramfs、默认启动项或 Android。替换程序时停止/启动服务会重建
  AFE/solver；这里的“不重置”只描述同一常驻服务中的两次采集，不包括部署本身；
- 采用 `/run/systemd/system/himax-touchd.service.d/90-live-capture-trial.conf`
  临时启用 10 秒窗口，实际服务的 StateDirectory、ProtectSystem/ProtectHome
  等沙箱限制保持。两个窗口中服务 PID 均为 447169、NRestarts=0，AFE epoch=7
  不变；没有因请求采集而重启服务、重新校准或重新打开 V4L2；
- 自动截止窗口：`/var/lib/himax-touchd-captures/live-XXebGzN9/capture.hxst`，
  560 帧、2903072 B；提前结束窗口：`live-XX5TsqFj/capture.hxst`，2 帧、10400 B，
  后者处于低频 IDLE。每个文件大小均为 `32 + frames*5184`，侧车 F 条数一致、
  `END frames 0 0`，两个 raw 首尾 epoch 均为 7，普通 raw 回放无拒绝帧。
  两份 `--session` 均按预期返回 1，明确提示没有 solver checkpoint；
- 该服务交接前累计 882 帧，bad/stale=0、touch_reports=0。这是无实际手势样本的
  首轮管线/IDLE 验证，不是触控质量、持续 120 Hz、峰值压力或延迟基准；
- 成功后将唯一临时配置移到证据目录保存，恢复原来无 live 选项的 ExecStart，再
  启动新 daemon。最终 PID=451070、epoch=9、588 帧时 bad/stale=0、NRestarts=0。
  **新用户态已安装，录制没有默认启用**；可选配置示例安装在
  `/usr/local/share/doc/himax-touchd/himax-touchd-live-capture.conf`，未来启用方法见
  运行中窗口录制章节；
- GPI/SPI/Himax 的运行 build ID 在部署前后逐字节一致，内核健康门控通过。
  daemon SHA-256：`72ce2c8d8ef5e515769622172d35263f24ffd060b9e23e7334fbbac3e1a1847d`，
  replay SHA-256：`50928ddf507a858e829eb98a296e9859c92fe31589eb4146cafd7f6e953ca15b`。
  也包含此前 `672988e` 的 checksum 分类对齐，不只是新增录制能力；
- 安装前重跑相关四项 host 测试通过，原提交完整 44 项 Release/ASan/UBSan 结果
  保留。没有增加固件命令或重测切频。真实有触点录制、物理充电/睡眠、异常现场、
  慢盘压力和录制开启时的触点时延仍待验收；
- 日志、部署脚本、两份 raw 回放结果与旧程序副本保存在
  `deploy-logs/live-capture-install-c00844e/`。临时配置保留为
  `completed-runtime-override.conf`，不是生效中的服务配置。

2026-10-23 AFE 异步初始化退避候选（尚未部署）：

- `himax_start_afe()` 改为单次完整硬件尝试，STREAMON 与恢复 work 共用
  `himax_start_or_retry()`；失败发布 stopped epoch、关闭 IRQ，再异步排队。
  不再在同一 op_lock 临界区连试三次，也不因一次初始化失败立即拆队列/重启服务；
- 退避来自固定上游 `DeviceRuntime::BringUpBackoff()` 的五档值，失败计数饱和，
  成功或新生命周期清零。停止回调先关 gate/清计数、解锁后才同步取消 work，
  迟到回调不能绕过 power/stream/initialized 检查；跨 epoch 的用户 reset 限频保留；
- 测试直接提取生产 startup/retry/STREAMON/STREAMOFF，覆盖 64 种 power 组合、
  检测及 power-on 连续失败、一次失败只做一次尝试、对应异步延迟、最终成功、
  停止后迟到任务、设备移除后的 STREAMOFF、OTP 二次 reset 与原 best-effort 命令。
  不模拟真实 kernel mutex/IRQ/workqueue 调度，不能据此声称并发/PM 已实机验证；
- Windows 退避原函数直接编译，完整源 SHA-256 锁定；近一万次连续失败及计数上限
  边界验证一致。提取门控检查原文完整、漂移/缺失拒绝、native 变更刷新，以及
  停止回调的计数清除/解锁后取消顺序。StreamState 另覆盖多次 failed epoch 不误报
  producer stall，以及进入 running 后仍必须持续产帧；
- 46 项 Release 和完整 46 项 ASan/UBSan 通过；`make -C tests test`、checkpatch、
  `git diff --check` 通过。编译服务器使用原匹配内核输出和独立
  `afe-retry-fosBox` 模块目录构建普通 V4L2，未改共享源码或已安装模块，构建无警告/
  错误；源码内容与本地逐文件校验一致；
- 候选位于 `artifacts/afe-retry-himax1/`，vermagic 为 `7.1.8-gaokun3-himax1`，
  模块 SHA-256：`1de7e614b3af24ab9eb5839f304022317975828cb2b8871ffd410be2dbb67da5`，
  build ID：`89b9985b76556d2179e11e83e6e1b426f2864efe`。**没有安装/加载此候选**，
  当前仍为 `44e6917` 模块和 `c00844e` 用户态；
- Windows 的 0x13 master/slave ready 前置探测、THP 句柄重开未映射为 Linux 操作，
  不因此启用笔或重绑控制器。未做真实 SPI 失败注入、工作队列取消竞态、睡眠唤醒或
  长期坏硬件退避验收；正常帧流、baseline、检测/跟踪算法没有改动，也未宣称本次
  修复已解释历史断触。下一步应受控试载、验证 healthy path，再验证初始化失败恢复。

2026-10-23 初始化退避实机测试准备：

- 增加上述诊断专用 pre-SPI 失败预算，最多八次，每设备一次性消耗，只在初始化
  路径执行。默认普通模块完全排除；重新构建的普通模块与前一节 `c1a8757` 候选
  逐字节相同（SHA-256 仍为 `1de7e614...b67da5`），不存在新增正常路径开销；
- host 测试覆盖注入先于所有 SPI/初始化调用、stopped epoch/IRQ、STREAMOFF 阻止
  迟到任务、新流重置退避但不重置故障预算，以及耗尽后恢复实际初始化路径；
- 47 项 Release 与 startup/stream 相关四项 ASan/UBSan 通过，checkpatch/diff
  通过；本次没有重跑完整 47 项 sanitizer。前一节完整 46 项结果属于当时版本；
- 编译服务器独立 `afe-inject-noqPJu/{normal,diagnostic}` 构建成功，两份产物取回
  `artifacts/afe-inject-himax1/`，vermagic 均匹配当前测试内核；普通模块不含注入/
  邮箱日志字符串或参数。诊断模块 SHA-256：
  `6597ef72d41e5916fb81bcb325da820937802ef9df9d785e619b698e976f6e46`，
  build ID：`ca062ef44a58952e499e9c57faf1d24bad33b661`。
  这些是试载前验证，不能冒充实机取消/恢复测试通过。

2026-10-23 AFE 退避实机试载、恢复与正式安装：

- 首先临时加载诊断模块 `afe_start_failures=8`。第一条流（PID 682901）在三个
  pre-SPI 主动失败后 STREAMOFF，期间没有 IC 检测或 raw-ready，停止后三秒内
  没有新增尝试日志。此时预算还剩五次，未强制制造物理 SPI 错误；
- 第二条流 PID 683456 一直保持运行，观察到剩余五次主动失败，退避从
  200/500/1000/1500/2000 ms 重新开始；预算耗尽后同一进程正常初始化、进入
  epoch=10 并产帧，期间无 watchdog/服务重启。内核短单调时间日志记录了实际
  workqueue 尝试间隔，但不作为精确调度延迟基准；
- 恢复会话共 287 帧，首帧 all-zero、286 帧有效；普通模块单独试载会话共 696 帧，
  首帧 all-zero、695 帧有效，stale=0。两份侧车分别为 `END 287 0 0`、
  `END 696 0 0`，严格会话回放 `live_output_verified=1`；均无真实触点报告，
  不代表手势质量验收，也没有把启动全零帧隐藏成“全程零错误”；
- 试载全部退出后先恢复原 `44e6917` 模块及常驻服务，核对运行 build ID 与原备份
  一致，GPI/SPI 未卸载。完整证据位于 `deploy-logs/afe-retry-trial-fba5d7a/`；
- 随后正式加载、安装不含任何注入/邮箱日志参数的普通模块，运行与磁盘 build ID
  均为 `89b9985b76556d2179e11e83e6e1b426f2864efe`，SHA-256 仍为上一节
  `1de7e614...b67da5`。未改用户态程序、算法、GPI/SPI 控制器或 Android；
  常驻 PID=720293、NRestarts=0，696 帧时 bad=1（启动全零）、stale=0、无触点报告；
- 重建 `/boot` 与 ESP 下同一 Himax 测试启动项的 initramfs，解包核对
  Himax/GPI/SPI/uinput 四份模块与磁盘一致。新 initramfs SHA-256：
  `bd97963c0ca912d37c62346da69fd07a21329d71fa3938e25d657ca1abb51f62`。
  kernel Image、DTB、BLS entry、默认启动项不变，DKMS mask 保留，没有自动重启；
- 安装/更新 initramfs 前后健康门控通过，无新 Oops/WARN/坏页。旧模块与两份旧
  initramfs 保存在 `deploy-logs/afe-retry-install-fba5d7a/previous-*`，部署脚本/
  服务统计也保存在该目录。新版已在当前运行，但更新后的启动镜像尚未重启验收；
- 这次实机验证证明的是主动 pre-SPI 失败时的异步等待、STREAMOFF 门控、预算耗尽
  后正常恢复及普通模块可运行，不是实际 DMA/SPI 失败、真实系统休眠/断电、初始化
  中途取消、异常触点/掌压或性能回归的完整证明。完整移植的其余验收缺口仍保留。

2026-10-23 IDLE 读等待许可对齐与用户态更新：

- 新增 `FrameReadMode`，按 Windows 的实际调用顺序在 EnterIdle 命令成功后立即
  允许长读等待，而非等 solver 首次看到 IDLE；有效 active 帧取消，坏帧/新 epoch
  清除，tagged 空轮询保留。统计 `read_idle_retry` 是 host 等待许可，不是 IC 的
  IDLE 完成位；仍使用 `afe_frames idle` 观察有效帧报告；
- 默认四秒 supervisor 路径和显式 `--experimental-rearm` 开关保持原样。
  65536 个 suffix word、命令分发失败及生命周期对照通过，Release 与完整
  ASan/UBSan 均 50/50；细节见
  [SPB 审计：EnterIdle 命令与读等待许可](../docs/windows-spb-backend-audit.md#enteridle-命令与读等待许可)；
- `039cdf0` 临时实验会话 25 秒，878 帧全有效，进入 IDLE 后未产生补读/复位，
  strict replay 878/878。随后已安装新 daemon，运行服务仍明确
  `producer_rearm=disabled`，877 帧时 bad/stale=0、NRestarts=0。
  daemon SHA-256 为 `9bfc6684f823ad35dd65596e629ad807c795a9c94171892bd0b968fd2d18721e`；
- 仅更新用户态程序，无内核源码变化，未重载任何模块；启动文件、unit 和 replay
  哈希不变。本轮不需要重建 initramfs，当前 initramfs 仍为
  `19fab49c4722da84a7d8d8387e06d15b7c09ee9c7ea66961845ef9cb5998a580`。
  备份与证据在 `deploy-logs/read-mode-install-2OwoOVHt/`；
- 没有真实触点、真实 lost-IRQ 或系统 suspend/resume 验收，不能将短期正常 IDLE
  通过称为历史断触根因已解决，也没有因此默认启用实验恢复。

### 2026-10-24：无首帧时的 AFE 重建接口（d75f89c 检查点）

固定上游 `DeviceRuntime::OnStreaming()` 在 `GetFrame()` 返回 SOS 后调用
`OnAfeSos()`、`BeginBringUp()`，保留同一个 solver/core 及进程；退出 streaming
时 `OnAfeStopped()` 并释放输入，重建成功才 `OnAfeStarted()`。当前 Linux 的
producer 超时耗尽仍退出 daemon，由 supervisor 重启，尚不是这条同进程流程。

新增 additive ABI v2 `VIDIOC_HIMAX_RESTART`（private +4）及
`Capture::Restart(epoch, sequence)`，只作为后续恢复闭环的基础：

- `hx_raw_restart` 为 32 字节，size/version/reserved 严格校验；原有 ioctl、HXST
  帧布局、flags 和 ABI version 不变，旧模块返回 `ENOTTY`；
- 仅队列所有者、已开始 streaming、所有 panel/system/shutdown 门控允许、AFE
  ready 且不在 startup cleanup 中才能申请。epoch 必须非零且精确相同，sequence
  也必须精确相同，但允许 0，覆盖“初始化后始终没有第一帧”的情况；
- 最近一次成功采集和成功主动读的完成时间都需至少静默 200 ms。与旧 frame-bound
  `HX_RAW_RESET` 共用一秒限频，开始新 epoch 不重置预算；
- owner/状态过期返回 `EBUSY`/`ESTALE`，尚未足够静默或触及限频返回 `EAGAIN`，
  shutdown 返回 `ENODEV`。不要求有空闲 buffer，不丢弃或重标已完成的旧帧；
- 校验和 IRQ 在同一 `op_lock` 下。IRQ 抢先产生新帧会使旧请求失效；重建请求先
  获准则关闭 IRQ/read gate、置 SOS、发 stopped event，并调度既有异步 AFE work。
  ioctl 自身不执行 SPI/reset GPIO，更不卸载或重绑定父 SPI/GPI；worker 才在重查
  电源门控后进行客户端硬件初始化；
- 返回成功只表示 **scheduled**，不是 recovered。旧 epoch 的 completed buffer
  仍保留原序号和时间，调用者必须处理生命周期及旧数据，而非当作新流求解。

当前常驻 daemon 没有调用此接口，四秒 supervisor fallback、默认关闭的实验 rearm、
250 ms 无新帧输入释放策略均未改变。新接口不添加正常帧采集/算法开销，也没有
以它替代尚未完成的 Windows 同进程恢复移植。

下一步接入必须同时完成：无帧触发请求的会话记录/严格回放、SOS/stop/start 调用
顺序、有界等待新 epoch 与第一份有效数据、`ESTALE/EAGAIN` 竞态处理和旧模块
fallback。先保留显式试验入口，再做实机正常 IDLE、故障恢复及手势回归，不把
调度成功或一份合法帧等同于触控质量恢复。

### 2026-10-24：同进程重建闭环（候选，默认关闭）

上节检查点之后，daemon 已接入独立的 `--experimental-restart`，继续保留同一个
Capture、uinput 设备、solver 和 TSACore 对象。未加此选项的常驻配置仍使用原有
四秒 supervisor fallback；它不隐式打开 `--experimental-rearm`。只开 restart 时
四秒耗尽后申请 +4；两选项一起开时沿用原 producer kick 预算再申请 +4。
observe/info 模式拒绝实验恢复选项。

请求受理后立即执行 `OnAfeSos → OnAfeStopped → input.Release`，在主线程本地
关闭旧 epoch，不依赖稍后 DQEVENT 才隔离旧 completed buffer。迟到的旧 ready
event/旧 packet 不能重新打开输入。后续新 epoch 仍经过身份检查、正常
`OnAfeStarted` 和 raw/AFE status 校验，保持同一个 core；新数据可用才增加
`afe_restart valid_after`。它不意味着真实手势质量已恢复。

单次竞态只允许一秒、100 ms 间隔重试，进度/epoch/sequence 改变会取消旧请求；
ENOTTY/其他致命错误退回 supervisor。受理后最多等待十五秒新可用帧，不因收到
stopped/ready 或坏包延长；等待中不再发嵌套 reset。连续三次受理后需要三十秒
持续好数据才补回预算，状态停止、坏数据或超过四秒的数据间隔打断稳定窗口。
这些是 Linux 防循环边界，不是上游 Windows 参数；真实 panel off 发生在受理
等待中也可能触发十五秒 supervisor fallback，当前生命周期 UAPI 未区分 stop 原因。
既有 250 ms 无帧释放行为保留。

fresh `--record` 改为 `HXSE 3 recovery`，Begin 记录实验选项；独立的
`J decision_ns completed_ns epoch sequence errno` 保存无帧请求，旧 HXSE 1 继续可读。
回放重建同样的 SOS/stop/start 和旧流隔离，核对输出/命令及请求 token/预算；
不把 host 回放称为物理 SPI 或超时原因验证。live 窗口仍无 checkpoint，不允许
严格 replay。完整语法见 [offload 录制说明](../docs/touch-offload.md#正常-afe-闭环录制)。

新增测试直接执行生产 `touchd` main、真实 TSACore、录制与回放，只替换 Capture/
input/桌面边界和主线程时钟。覆盖健康/默认不开启、触点在场停帧、无首帧、两次
EAGAIN 后受理、重建后的前五帧坏包、旧模块 ENOTTY、永不 ready 和持续坏帧。
同一流只 Open/Start 一次；重建后旧包/旧 ready 事件被隔离，失败有界退出。
另有独立 policy 纳秒边界/重复故障测试，以及真实 solver 会话的 token/时间/策略
元数据损坏反例。这些都是离线边界模拟，尚未证明真实停 IRQ 后恢复成功。

此候选未安装到本机，未修改 unit/initramfs、SPI/GPI 或 Android；普通常驻版本仍为
前面的两帧队列部署。下一步是配套模块的受控试载、真实恢复证据和手势/PM 回归。

### 2026-10-24：已安装并开启同进程重建

上述候选随后在本机试载并安装。正常实验会话 878 帧有效，无额外重建；受控 IDLE
下的三次 +4 请求均进入新 epoch，每次恢复有效 raw 和 calibration-done，首份
有效数据约在受理后 425～437 ms。这是主动重建验证，不是 lost-IRQ 恢复证据。
临时工具未纳入源码，原始证据保存在 `deploy-logs/restart-trial-w6I3dojw/`。

常驻 unit 现显式开启 `--experimental-restart`；主动 rearm 和窗口录制仍关闭。
fresh 采集脚本与可选 live-capture drop-in 使用相同重建设置。普通模块 build ID
为 `a74a57ee65c444d38a481f14ae48213e8f31b1a0`，daemon/replay 为 `b1fc40b` 构建。
两份测试 initramfs SHA-256 均为
`8ef6d45bbcc963d89becfe01e752572c7cf41fbd34c5def73bfc1dff1f9c7411`；
解包的 Himax/GPI/SPI/uinput 与磁盘一致。未改 Image、DTB、默认启动选择或 Android，
没有自动重启；新镜像冷启动及真实触点/PM/故障恢复仍待验收。
旧模块、程序、unit 和启动镜像备份在 `deploy-logs/restart-install-UV120Lpk/`。

随后已安装上述 `himaxctl` 入口。实机 reset 前后保持 PID=2688416，
epoch 3→4，重建后 sequence=91 并继续产帧，bad/stale=0、无服务重启。
手动 calibration 的传输已确认，短观察窗口没有新的完成标记，未将它记作校准完成。
本轮只更新用户态和 unit，内核模块、SPI/GPI 运行 notes 及启动镜像保持一致；
旧程序和配置备份在 `deploy-logs/operator-install-B393RjMh/`。

随后补齐 `--wait` 和校准状态并部署。实机在 IDLE 中下发手动校准后，已观察到新
completion marker 和正常清状态握手，修正了前次仅观察半秒而“尚未确认”的记录。
同一 PID=2754990 下 `reset --wait` 从 epoch 6 进入 7，确认有效帧后才返回成功。
没有新增测试代码或改变内核/启动镜像；旧程序保存在
`deploy-logs/operator-feedback-2PoDRond/`。这些结果证明命令/回执闭环，不证明
任意触控异常都能被校准或重建修复。

### 2026-10-24：常驻服务启用两级读帧恢复

常驻服务、可选 live-capture 配置及 fresh 采集脚本现均启用
`--experimental-rearm --experimental-restart`：先有界无复位补读，预算耗尽后
同进程重建 AFE，失败再交给 supervisor。`himaxctl status` 增加两级恢复的
开关及计数，便于区分正常产帧与自动恢复；正常触控算法未改。

已编译并安装用户态程序及配置，未更换模块或启动镜像，没有新增测试代码。
安装时屏幕关闭，未触发补读或重建；随后自然亮屏，同一 PID=2818725 收到
2833 帧，bad/stale=0，两级恢复计数及 NRestarts 均为 0。这只确认当次正常
产帧，不代表真实停帧恢复或边缘手势已验收。备份与安装记录保存在
`deploy-logs/read-recovery-install-U19x7ZPG/`。

随后补齐停止原因并部署配套模块和用户态：PID=2929330，912 帧有效，
bad/stale=0，`stop_flags=1`，两级恢复及服务重启均为 0。服务器模块构建、
用户态构建及相关的四项既有检查通过。两份测试 initramfs 已同步并核对模块；
未冷启动，也未主动触发系统休眠或恢复中关屏。备份及记录位于
`deploy-logs/power-stop-install-2z6wnlGI/`，本轮不改 SPI/GPI、启动选择或 Android。
