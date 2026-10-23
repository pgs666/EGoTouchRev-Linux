# HX83121A 逆向证据审计与后续移植路线图

## 2026-10-23：默认架构已经改为用户态移植

本仓库为 **EGoTouchRev-Linux**；`third_party/EGoTouchRev` 是固定在
`4d4f53d`（v1.4.0）的上游算法子模块。默认链路现为
`SPI → V4L2/HXST → himax-touchd → TSACore → uinput`。算法编译、面板参数、AFE
闭环和回放入口以 [用户态运行说明](../userspace/README.md)、
[Offload 架构](touch-offload.md) 和实际源码为准。

**下文的 2026-08-25 审计与实施顺序是 legacy 内核算法的历史计划。**
不能用其中的“三份 baseline”“单面板 profile”“速度驱动 EMA”描述当前默认求解器，
也不应再次在内核中重写已经导入的上游模块。默认构建排除了旧 baseline/tracking/filter；
旧路径仅在 `HIMAX_V4L2=0` 时编译。

当前移植边界：

2026-10-23 早先实机阻塞：`ddaba62` 的严格 SPI 长度检查未能安全热部署。
单独热替换 GENI 控制器触发原 GPI DMA 配置生命周期缺陷，内核已出现两次 Oops；
此前“raw 帧正常、脚本返回 0”的通过结论撤回。当时恢复原控制器和 `f27e70e`
触摸模块，遗留实验控制器覆盖文件已隔离。
[SPI 计数与 GPI 生命周期两项修复](../kernel-patches/README.md) 必须随完整内核经重启验证，
不能继续通过控制器热替换试错。新增离线/部署门控测试不等于真实内核安全验收。

同日后续：已在编译服务器从本机对应源码构建两项修复的完整内核
`7.1.8-gaokun3-himax1`，并编译匹配的 V4L2 模块；产物已取回并完成校验、
模块依赖检查和修复函数反汇编核对。随后已作为独立测试启动项安装，initramfs
中的匹配模块核对通过。用户已重启进入该版本，运行 GPI/SPI/Himax build ID 一致，
本次启动未见 Oops/坏页/Himax SPI 错误；两次统计帧数 6853→7985、bad=2 不增长、
服务零重启。仅证明本次启动与短期数据链路，不是长期稳定/真实睡眠验收。
构建/部署证据与验证边界见 [宿主补丁记录](../kernel-patches/README.md)。

当前工作进一步对齐 master 读取：一次 5063 字节 SPI 事务取得 5060 字节 native
master，HXST 记录保持原容量，尾部零填充且有效长度明确。新用户态兼容新旧长度，
旧用户态不接受新模块。`44e6917` 随后已在该测试内核部署：五秒采集 514 帧，
首帧全零、后续 513 帧有效；常驻服务统计 1798 帧、bad/stale=0，产生 443 次
触摸报告。测试项 initramfs 已同步，但尚未重新启动此更新后的镜像；这些短期
结果不等于快速入屏手势/休眠唤醒/长期验收。

后续校验审计：移除 Linux 独有的固定 `5a a5` 帧头门槛；新增直接编译固定 Windows
`CheckFrame` 的 oracle，保留长度、checksum、全零和填充检查。历史六份采集的
3214 帧接受/拒绝集合不变，不能据此声称修复断触。该修改已随 `c00844e` 用户态部署。
IRQ/清帧仍有平台边界：Windows 消费 THP 缓存并重开中断通道，Linux 是直接
threaded IRQ 读硬件；四秒超时、混合错误五帧阈值不是 Windows 双通道恢复的
逐项等价实现，需继续审计和故障验证。

后续已直接静态审计本机 Windows `SpbThpTool.sys`，详见
[SPB 后端证据与移植边界](windows-spb-backend-audit.md)：确认 ACPI/Resource Hub
绑定、两帧 FIFO、IRQ 生产者，以及 `IntOpen` 的无复位补读行为。两个固定名称不能
证明两个物理片选；旧逆向文档中的“无 ACPI、DT 应建双节点”不再作为依据。
随后已实现 master-only 无复位 producer kick 的门控/竞态和有界恢复策略，而不是
用重启 daemon 或热重载父 SPI/GPI 模拟 `IntOpen`。生产函数及固定 Windows
重试 lambda 的离线对照已接入测试；模块在编译服务器构建完成，经过两轮临时试载
后已配套修正 daemon 部署。正常 IDLE 的补读返回过全零，后续同 epoch 的正常 IRQ
继续有效；不能据此宣称丢中断恢复。测试启动项 initramfs 已同步，尚未重启验收。
真实停 IRQ/恢复、PM 并发和手势回归仍待验收。

再下一项已补 master bus-ready 前置检查：开始完整 reset/OTP/AFE 之前读 `0x13`，
零响应只等 500 ms，读取失败退避，不把“尚未供电”直接变成重复初始化。
生产启动函数与固定上游 `ProbeBus/check_bus` 直接编译对照通过；
普通模块已通过启动/显式 reset 试载并安装，测试启动项 initramfs 同步。
它只满足 master 非零条件，
不等于 Windows 双通道和 handle reopen 语义已全部移植。

后续日常运行发现新的阻塞：连续五次主动补读交付全零，触发一次 raw health reset。
缺少该时段同步 raw，尚不能判定是真实 producer 故障还是主动补读对静默的误判；
之前无触摸的短期通过不能推广成恢复策略验收。已保留新 kernel/bus-ready，
将 daemon 回退到 `c00844e`，不再自动发 kick。下一步需要区分主动空轮询与正常 IRQ
异常帧，审查 active→IDLE 过渡的等待策略及 strict replay 记录后再重新启用。
HEAD 同时改为显式 `--experimental-rearm` 才使用新策略，默认构建/服务保持四秒
等待路径；当前没有以开关替代实现剩余工作，也没有宣称恢复移植完成。

后续代码增加独立 tagged-rearm ioctl 和逐帧来源，raw/session reader 与 strict replay
均保留它。带来源的全零轮询不计入 IRQ 错误阈值、不刷新 producer deadline，真实
IRQ 错误及非全零的主动读错误保持原规则；宽限从 ioctl 完成后开始。合成八个空轮询、
丢失来源的回放反例、慢 SPI 完成后的截止测试已接入。这是修正已知语义冲突，
并非证明 active→IDLE 过渡或真实丢 IRQ 恢复已解决；实验模式继续默认关闭。
`1674f8e` 受控试载确认主动空读带 REARM、后续同 epoch 的 IRQ 清除 REARM；
随后配套模块/daemon/replay 已安装，常驻服务保持 `producer_rearm=disabled`。
更新后的测试 initramfs 尚未重启验收。

接着审查 active→IDLE 过渡，确认 Windows `EnterIdle()` 在命令传输成功后直接
`ArmIdleRetry()`；该读等待许可独立于帧尾确认的 AFE mode。Linux 新增独立的
`FrameReadMode`，成功命令立即允许 IDLE 长等待，合法 active 帧取消，普通坏帧/
新 epoch 清除，带来源的空轮询保留许可；没有伪造 solver 的 IDLE 状态。
全部 65536 个 idle word 与两种前置状态直接对照固定 Windows 帧尾代码；
实际 native/Windows 命令分发另覆盖五槽、事务失败和已有许可。
Release 与完整 ASan/UBSan 均 50/50 通过。已有正常 878 帧实机记录的离线时序检查
显示一次命令先于帧尾确认、最大 IDLE 交付间隔约 3050 ms，新策略未预测补读，
strict replay 878/878 一致。这不是历史异常根因或真实 lost-IRQ 恢复证明，
自动补读仍必须显式启用；详情见 SPB 审计的读等待分节。

用户随后实际采集六次滑入，发现一条本地首点补偿的误拒绝：两个初始坐标均被夹在
边界时，三点方向检查把零法向位移当成无效运动。修正为只接受确切边界的这类前段，
屏内停顿/反向/突转等继续拒绝。2091 帧原记录与实时输出严格一致；
新旧离线 A/B 中第三、第五、第六次首点由 44.9/43.1/37.1 补回 0.2 面板像素，
报告时刻和全部 ID/事件/压力不变。52 项 Release 与完整 sanitizer 通过，
这仍是本地修正与离线证据，不冒充上游原样实现或实机手势验收。

随后已实现异步 master startup cleanup 候选：先打开 producer，再由 IRQ 消费三次
启动读取尝试；200 ms 等待超时才 kick，清理结束后发布 consumer-ready，停止/
休眠取消。成功主动读的完成时间另作 IRQ 去重与静默门控，覆盖慢 SPI 中途 IRQ
到达，保留原始采集 timestamp。52 项 Release、完整 sanitizer、legacy 回归及
checkpatch 通过；编译服务器匹配内核模块构建成功，尚未加载/部署。
仍未复刻 Windows 双通道两帧 FIFO，不将 packet 当场消费宣称队列调度完全等价。
本机保持 `04f904d` 边缘修复供用户复测；候选详情和待验收范围见 SPB 审计。

该候选随后两轮普通模块临时试载：869 帧 fresh session 严格回放通过，188/187 帧
的启动/三次 reset raw 全有效，每次新 epoch 首个有效 sequence=1 且看到校准完成。
STREAMON 返回后、尚未 ready 时的 STREAMOFF 取消验证无迟到 running/清帧完成，
随后重开与 reset 正常。十次正常清理都是三包、零超时；真实超时/SPI fault 和
PM/DPMS/手指在场尚未验收。试载均已恢复旧客户端，未持久安装或改 initramfs，
SPI/GPI 从未卸载；不将正常启动成功当作完整移植完成。

| 范围 | 当前状态 | 尚需证据或实现 |
|---|---|---|
| 手指求解 | 直接编译固定版本上游的所有实际源码模块、adapter、solver、参数表和 mapper | 不等于恢复了上游自身裁去的原厂私有功能；仍需真实多指、边缘、快速滑动验收 |
| AFE 生命周期与闭环 | 内核负责 reset/reload/raw runtime；用户态执行清状态、跳频、扫描率、校准和 IDLE 请求并回报结果；生产邮箱协议与直接编译的 Windows 构包函数做全字节域对照，并覆盖五槽三步事务失败重试；本机显式校准、清状态、IDLE 的后续 suffix 已确认 | 本机频点 1 命令虽传输成功，五秒内未见频点或完成位变化，不能算跳频通过；扫描率只有传输证据；仍需实际系统 suspend/resume 和故障恢复回归，模拟事务失败不等于真实 SPI 故障注入 |
| 系统电源与充电 | 独立系统 PM 状态、统一硬件入口检查、shutdown 停止；充电观察/应用状态分离并失败重试，reset 限频跨 epoch 保留 | 状态测试、真实命令与 DPMS 已验证；真实系统睡眠、物理充电切换及充电写失败实机注入仍未验收 |
| 面板与坐标 | OTP 选择参数；固件坐标接线标志映射到 uinput；本机读出 `W273AS2700` | 其他面板实机；各方向触点与显示位置、握持区域映射 |
| 宿主方向 | 独立 KDE 桥接、三秒方向缓存和非阻塞启动握手；两处经过源文件哈希检查的构建适配在初始化及复位内部应用宿主方向，不借用笔状态、不重置触点或 AFE | 握手不是首帧方向必达保证；仍需实际四方向手势、其他桌面的方向生产者，以及上游更新后的适配复审 |
| 边缘起点与内存边界 | 首次获准上报时做有界滑入起点修正；候选已越过窄 ER 带时要求紧邻的前置局部贴边脉冲及速度/时间一致性；修复轮廓最后一行/列插值越界；生成副本及本地实现均明确区别于上游 | 240 组合成 A/B 与屏内起始对照不等于实机手势验收；靠边按下再朝内移动仍可能被识别成滑入，缺少边界采样/信号或来源不明时不补偿 |
| 诊断与回放 | HXRC/HXST、旧 raw/trace；fresh 会话异步录制与严格输出回放；可选 SIGUSR2 运行中窗口已部署并验证自动/手动结束，明确标注无 checkpoint；默认关闭 | checkpoint、真实有触点/压力与性能验收；真实异常现场与 Windows 同操作序列比较 |
| 主动笔 | 按用户要求保持关闭，不注册 pen input device | 独立项目，不能通过手指移植顺带启用 |
| Android | 现有 Android 构建/工具保留旧架构 | 新 daemon 的交叉编译、init/SELinux、uinput/显示桥接与诊断 APK 集成 |

AFE 分发审计补充：新增直接编译 Windows AfeController 公共方法和实际 native
dispatcher 的测试，验证 `HardwareAction → command/value → mailbox` 在支持参数、
五槽及每步传输故障上的一致性；生产 dispatcher 的参数白名单保持不变。42 项
Release 与相关 4 项 ASan/UBSan 通过，没有发现切频命令号/参数映射错误。
这不是固件完成证据，也没有解除表格中的跳频实机验收缺口；默认运行代码未改。

运行窗口部署时：用户态更新到 `c00844e`，当时模块为 `44e6917`。临时配置下两个
运行中窗口保存 560/2 帧，同一 PID、同一 epoch=7、零丢帧/错误；无真实手势报告，
不能当作触控质量验收。已恢复无录制选项的正常服务，未改内核/启动镜像/Android。
运行中抓取保持状态，与部署时必要的服务重启不同；详细边界见用户态运行说明。

生命周期移植与最新部署：已将固定 Windows `BringUpBackoff()` 的
200/500/1000/1500/2000 ms 与一次 Init 尝试策略接入原生 delayed work。STREAMON
初次失败不立即拆队列；停在 stopped epoch 后异步重试，成功后才开 IRQ/产帧，
重复/迟到回调重查 power gates，不重绑任何控制器。普通模块已在测试内核安装运行，
并同步 initramfs；诊断模块仅试载后卸载，没有持久安装。pre-SPI 主动失败预算实测
STREAMOFF 后停止继续尝试、同一新流随后正常恢复；两份会话 287/696 帧，各有一帧
启动全零，其余有效，严格回放通过，无实际触点报告。试载与部署详情见用户态说明。
回归测试使用实际生产 startup/retry/STREAMON/STREAMOFF 函数及未改写的 Windows
退避函数，不替代真实 SPI 故障或系统 PM 的并发测试。0x13 前置门控和 THP 句柄
重开仍是未对齐边界，不能把这一步称为恢复路径完整等价。

接下来优先验证默认链路的行为等价与故障边界：先记录真实手势、屏幕方向和 AFE
动作/完成状态，再做实际睡眠唤醒、充电、多指和长期性能回归。对比对象是固定版本的
上游 Windows 重写实现；掌压亮屏问题不能仅凭“导入上游代码”宣称修好。
上游未启用的 SafeBaseline/SideTouch 等功能，不应以“完整移植”为由自行打开。

初始化审计补充：默认 V4L2 入口通过共享 `hx_raw_start_scan()` 执行 Windows Init
实际调用的 reload/inspection unlock/sorting/两段 N-frame 准备，再执行 power-on、
完整 reload word 轮询、最多 50 次 raw 类型校验和 SRAM producer 握手。已移除默认
路径额外的 A8 ready gate、power-on sorting 读回和后置 legacy AP/N-frame/raw 重放；
Linux 充电状态仍由独立 work 应用。旧检查和 replay 只保留在 legacy 入口。
测试直接编译上游八个原始函数体和 Init 的 master 调用片段，比较实际事务、延时和
故障/重试结果。默认 AHB 原语另接入 `hx-ahb.c`，使用 Windows `0x12/0x13`，
移除寄存器事务结束的反向 burst 切换，并对齐 interface-on 轮询与错误返回。
AHB oracle 直接编译上游总线协议函数，对照实际命令/地址/payload/停止位置；
启动 reset 与 sense-on 也已接入共享 AHB 层及上游原函数对照。默认入口清 reload 后
切 reset、开 burst、等 10 ms、只读 IC ID；首次 OTP 后再 reset。不再每次读 IC ID
都进入旧 safe mode，OTP 自己的安全模式不变。probe 先释放 GPIO，避免 reset 中
执行第一笔寄存器写入。native SPI 封包现有独立对照：直接编译原生读写函数与
Windows `IoctlOnce/Ioctl/ReadBus/WriteBus`，比较 256 个命令、地址/payload、读取
十次重试、短读取和取消；native 另校验实际读写长度、终止控制器停止/移除重试并
重建被 RX 覆盖的重试 header。测试替换 OS I/O 边界，不覆盖物理 SPI/Windows
overlapped/THP IOCTL，短写拒绝和 Linux 专属终止错误也不是上游逐项等价行为。
实机另发现 GENI GSI 提前完成消息，导致成功返回但长度为零；严格 SPI 检查依赖
[宿主控制器补丁](../kernel-patches/README.md)，不在 Himax 客户端中放行零长度。
这不代表 Windows 整个 `Chip::Init()` 已经逐笔等价：只读 IC 检查、
GPIO 时序、IRQ/清帧及 Linux 充电集成仍有差异，验证边界
见 [用户态运行说明](../userspace/README.md)。

`33b01f5` 已经完成本机普通模块试运行及正式安装，运行模块与磁盘/DKMS 缓存一致。
32 项 Release 测试、AHB 相关两项 sanitizer、三种模块构建及 basic AFE 检查通过；
显式校准、清状态、IDLE 可确认。带诊断模块 full AFE 重测中频点 1 仍未观察到完成，
五秒后 sequence 638 仍为 slot=0/done=0，测试返回 1 并恢复旧模块。保留失败结果，
不能把 AHB/初始化事务对齐当作切频恢复的证明。
reset/sense-on 和 IC 探测入口已完成本机正式安装（`f27e70e`），native SPI 封包、
实际传输长度和重试已补充实现/host 对照，后续仍需受控实机验证以及清帧与 IRQ 审计。
寄存器级与总线命令/GPIO回调级 oracle 都不覆盖物理 SPI/IOCTL 差异，
不能仅凭帧流正常就宣布完整移植完成。

---

本文基于当前 Linux 驱动、Gaokun 原厂 Windows 二进制逆向成果，以及社区
`EGoTouchRev-Windows` v1.1.2 的 clean-room 重写实现，重新判断哪些能力已经落地、
哪些仍值得移植，以及正确的实施顺序。

审计基线为 2026-08-25 的 Linux `main`（`d6696b3`）。本文是路线图，不是对原厂
TSACore 的兼容性承诺；后续代码变化应同步更新“当前实现”章节，而不应继续沿用历史
分支状态。

## 1. 证据来源和使用规则

本项目现在有四类性质不同的参考，必须分开表述：

| 等级 | 来源 | 能证明什么 | 不能证明什么 |
|---|---|---|---|
| A | `HuaweiThpService_RE` 中的反编译、反汇编、参数 dump | Gaokun 原厂二进制实际存在的寄存器、调用顺序、状态机、参数和日志 | 反编译未还原字段的精确语义；是否与零售版 Windows 触控栈完全相同 |
| B | `HuaweiThpService_RE/*.md` | 对 A 类材料的可读总结和交叉索引 | 文档中的推断不能替代对应反编译代码 |
| C | `EGoTouchRev-Windows` v1.1.2 | 社区 Windows 重写版的完整可读算法和产品化工程经验 | 它不是 Himax/Huawei 原厂源码，不能把其行为称为“Windows 原厂行为” |
| D | 当前 Linux 代码、host tests 和实机日志 | Linux 已实现行为和本机实测结果 | 未覆盖场景下的可靠性和原厂等价性 |

本地参考路径：

```text
/home/user/Project/HuaweiThpService_RE/
/home/user/Project/EGoTouchRev-Windows/        # v1.1.2: a6867ed
```

重点材料：

- `08_himax_drv_deepdive.md`：HX83121-A 初始化、帧流、邮箱和恢复；
- `10_apdaemon_deepdive.md`：采集线程、SOS、日志和扫描模式管理；
- `11_tsacore_deepdive.md`：原厂触控流水线、参数偏移、基线、坐标和异常处理；
- `13_tsaprmt_pcomm_deepdive.md`、`14_tsaprmt_project_index.md`：四种 Gaokun
  面板 profile 的索引和原始参数；
- `decompiled/TSACore.dll.c`：SafeBaseline、BLReset 等未被深度文档完全展开的直接证据；
- `EGoTouchRev-Windows/docs/touch_pipeline_architecture.md`：社区 v1.1.2 管线说明。

发生冲突时，按 A → D → C → B 的顺序核对具体问题：硬件协议优先看原厂二进制，
当前行为优先看 Linux 代码，算法设计可参考社区 Windows 实现，摘要文档只用于导航。

## 2. 对旧路线图的关键修正

1. `EGoTouchRev-Windows` 是 clean-room 社区重写版，不是“Windows 原厂源码”。它的
   BaselineTracker、TouchTracker 和 1-Euro 可以移植，但只能表述为 C 类参考。
2. 原厂 `thp_afe_start()` 已确认执行 reset、`hx_sense_on(1)`、MCU/reload 等待及
   master/slave 各清三帧；没有证据表明 screen-on 必然执行 calibration。社区 Windows
   v1.1.2 的 `Chip::Init()` 会发 calibration，但这不能反推为原厂要求。
3. 当前 Linux 的 `working / wake_candidate / safe_baseline` 是为已观测故障设计的最小
   SafeBaseline 闭环，不等于完整移植原厂 SafeBaseline/BLReset。
4. 原厂二进制确实以 `BaselineQueue_Init(..., 5, ..., 5)` 建立容量为 5 的队列；但旧文档
   所写的全部元数据含义尚未逐字段验证，不能据此直接设计 ABI。
5. 当前 Linux 的 `euro_*` 实现是“按瞬时速度线性选择 alpha 的定点 EMA”，没有使用
   帧时间、标准 1-Euro 的导数截止频率和 cutoff 公式。因此它不是完整 1-Euro。
6. 当前 tracking 已经包含 persistent peak ID、Hungarian、split competition age、
   handoff residual 约束和 silent gap。继续把 tracking 当作亮屏断触的首要病灶已不合理。

## 3. 当前 Linux `main` 的真实实现

### 3.1 生命周期和亮屏恢复

默认 `full_resume_on_panel_enable=1`，panel enable 后执行：

```text
延迟等待面板电气状态稳定
  -> hardware reset / IC detect / safe mode
  -> 允许 firmware reload
  -> normal mode + N-frame
  -> sense on
  -> 等 0x900000a8 byte0 == 0x05
  -> 等 0x100072c0 == reload done
  -> 设置 raw type 0xf6 + raw SRAM handshake
  -> 尝试恢复 120 Hz
  -> fresh-frame/hash 检查
  -> wake raw-quality qualification
  -> 开 IRQ
```

核心寄存器握手与原厂路径已经对齐，但不是逐条复制：Linux 只有一个可见 event-stack
端点，没有照搬 Windows 的 master/slave 各清三帧，而是读取变化帧并对 raw 网格做质量
判定。retained warm resume 仍保留为显式 A/B 逃生路径，但不是默认路径。

自动 calibration 已从默认恢复路径移除；当前邮箱读回只能证明命令传输完成，不能证明
calibration 或 scan-rate 状态机已经完成。

### 3.2 baseline 和异常帧

当前已有：

- Q8 逐节点 working baseline、common shift 估计、触点冻结、release hold 和分级追赶；
- `safe_baseline_q8`、`wake_candidate_q8`、`baseline_q8` 三份网格；
- 无手指稳定窗口提交 safe baseline；
- 唤醒时有手指则使用 last-safe，首次启动没有 safe 时使用受保护的中性 baseline；
- candidate 与 last-safe 空间差异过大时要求第二个稳定窗口；
- 整行/整列或大面积异常帧保持上一触点，持续异常后完整 reinit。

这已经解决了旧实现“硬件重置后直接信任旧 baseline”和“只检查 SPI 格式、不检查 raw
质量”的主要缺陷。尚未实现原厂 BLReset 的完整异常触摸、mutual/self 不一致、长期负信号、
raw jump、clean baseline 和 safe queue 状态机。

### 3.3 触点求解和 tracking

当前已经具备：

- 8 连通 macro-zone、容量溢出时保留强候选；
- palm 多证据评分、pressure-drift、Z8、edge 和 close-saddle peak 过滤；
- persistent peak ID 和基于运动预测的匹配；
- BFS/局部加权质心、边缘信号增强、线性边缘坐标外推和弱边缘拒绝；
- Hungarian contact assignment、速度预测、动态 down debounce；
- firmware finger rising 驱动的可信边缘首触快速上报；
- continuation-only 小峰、split/cross-zone 确认、handoff residual 防重复 slot；
- invalid/retry/SPI 失败帧不推进 `missed`，固件明确 lift 时立即释放；
- RX ghost filter 和最多 4 帧的已上报轨迹 silent gap。

当前求解器不是原厂 TSACore 的逐模块移植。尤其缺少完整 `TZ_Process`、`ER_Process`、
`TS/TE`、四边独立 edge correction profile、SignalDisparity 和 BLReset 联动。

### 3.4 诊断和性能边界

`HIMAX_DIAGNOSTICS=1` 才编译完整 event stack、矩阵和 track ring；普通构建只保留自动
恢复所需的轻量状态。诊断版用于现场定位，不应作为性能基准。

当前普通热路径已删除无消费者的内部 gesture 状态机、默认关闭 Grid IIR 的逐帧历史复制、
重复 track 计数和若干无效 scratch 清零。后续性能工作见 [性能审计](performance.md)。

## 4. 当前实现与参考实现的差距

| 子系统 | 原厂逆向证据 | 社区 Windows v1.1.2 | 当前 Linux | 判断 |
|---|---|---|---|---|
| screen-on | full start、MCU/reload 等待、双端清帧 | 完整 `Chip::Init()` | 完整 reinit + 单端 raw qualification | 核心闭环已完成，保留实机回归 |
| calibration | 独立命令和 done 状态；未见于 `thp_afe_start` | 每次 Init best-effort 发命令 | 默认不发 | Linux 当前更保守，不应为“对齐”而恢复 |
| 帧健康 | checksum/all-zero/same-frame/超时计数后 SOS | 有重连和恢复状态机 | SPI 连败与粗粒度 noise 恢复 | 缺 invalid/retry、重复、全零和停滞看门狗 |
| baseline | BLIIR + BLSM + BLReset + SafeBaseline queue | 单 working BaselineTracker | 三份最小 SafeBaseline | 唤醒闭环可用，运行时 trustability 仍是子集 |
| 面板 profile | 1300/1310/2700/2710 四套参数 | 社区默认值为主 | 单套通用默认值 | 高价值缺口，必须先识别面板再导入 |
| 坐标 | TZ + ER 二次/三角/质心 + 四边修正 | flood/centroid + LUT edge | flood/local centroid + 线性 edge push | 当前平滑度和边缘精度的主要缺口 |
| 时间滤波 | 多个 post-filter/动作模块 | 带时间戳和导数低通的浮点 1-Euro 变体 | 速度驱动 EMA | Linux 名称和能力不一致，需要独立 A/B |
| tracking | Peak ID、IDT、动作历史 | Hungarian、GapRelink、加速度门控 | Hungarian、peak ownership、简单 gap hold | 已较强；只按 trace 补缺口 |
| 异常噪声 | charger/RX、SD、raw jump 等分类并保持上一触点 | 自定义 CMF/ghost | 线/面阈值 + hold | 应先诊断分类，再允许影响上报 |
| 跳频/扫描率 | TSA 决策，AFE 执行并回报状态 | 固定 120 Hz 为主 | best-effort 120 Hz | 动态跳频仍是高风险可选项 |

## 5. 固定实施顺序

### 阶段 0：冻结当前可靠性基线并建立量化数据

在改变坐标或阈值前，保存当前普通版和诊断版的模块哈希、默认参数以及一组固定 raw
fixtures。实机覆盖：

- 冷启动和至少 30 次连续亮灭屏；
- 亮屏前手已放在屏上；
- 充电/电池、日常/游戏 profile；
- 屏幕四角和四边慢速/快速起触；
- 快速单指重复滑动、双指靠近/交叉/分离；
- 静止点、横线、竖线、斜线、圆形。

诊断记录至少能关联 raw frame、peak/contact、slot、输出坐标、baseline 来源和恢复原因。
为平滑度建立离线指标：静止标准差、直线正交残差、圆拟合残差、首触距离、端到端帧延迟。

完成标准：同一 trace 能在 host 工具中重复得到相同触点和指标；当前版本成为明确的回退点。

### 阶段 1：补齐运行时帧流健康看门狗

这是下一个可靠性优先项，不改正常帧算法。增加：

- consecutive invalid master / retry streak；
- event-stack hash 或 raw 网格长期不变，但 IRQ 仍持续到达；
- 全零/固定填充值和不合理范围节点；
- firmware `has_finger` 与 raw/track 长期矛盾；
- 每种原因独立计数、首次时间、连续长度、恢复结果；
- 分级动作：单帧 hold，短 streak 只记录，持续异常才完整 reinit；
- reset storm 的退避、时间窗限频和失败后 IRQ 状态说明。

不要把“正常静止手指导致 raw 变化很小”误判为卡帧。freshness 必须结合完整 event-stack、
IRQ 时序、suffix/status 和允许的 raw 小变化判断。

完成标准：注入 invalid、retry、SPI error、全零和重复帧时不产生单帧 UP；持续故障自动恢复，
日志能唯一说明触发原因。普通构建只保留 O(1) 计数和必要 hash，完整证据仍由诊断开关控制。

### 阶段 2：建立面板识别和版本化 profile

先只建立数据模型和可观测性，不立即替换全部默认值。原厂四套 Gaokun profile 为：

| project ID | 面板 | 参数文件 |
|---|---|---|
| `W273AS1300` | BOE old mapping | `GaokunHimaxBOE.bin` |
| `W273AS1310` | BOE new mapping | `GaokunHimaxBOE_NewMapping.bin` |
| `W273AS2700` | CSOT | `GaokunHimaxCSOT.bin` |
| `W273AS2710` | CSOT new | `GaokunHimaxCSOT_New.bin` |

实现要求：

- probe 时只读一次 OTP/project ID，并缓存选择结果；读取失败回退当前通用 profile；
- 把 profile 写成有版本、字段名、单位、来源偏移和范围检查的 C 结构；
- 先导入几何、`fingerThold`、`sigMult*`、`accThold`、四边 correction ratio、
  `cmfSigThold/cmfSigDiffThold` 等已确认字段；
- 不把 6288/6336 B blob 直接暴露为 sysfs ABI，也不在 IRQ 中解析 blob；
- 注意原厂 dif 极性、raw normalization 和单位与 Linux 不完全相同，数值不能机械照抄；
- 未确认版权和再分发条件前，只提交字段化的必要标量及来源说明，不打包完整 vendor blob。

完成标准：diagnostics 明确显示 project ID/profile/revision；未知面板安全回退；profile 切换本身
不改变 slot 生命周期，逐组 A/B 导入参数。

### 阶段 3：重做空间坐标求解，先内区、后边缘

这是改善斜线、圆形和静止抖动最值得做的算法工作。分两次提交：

1. 内区坐标：在已确认的单峰紧凑触摸区中，对 X/Y 投影使用有界 Q10 三点/抛物线插值；
   分母过小、邻点非单峰、区域被分裂或数据越界时回退当前加权质心。
2. 边缘坐标：使用每个 panel profile 的 top/bottom/left/right 独立修正；保留 raw centroid
   和 corrected coordinate，禁止外推越过物理显示范围。

不要声称这是完整 `ER_Process`，除非继续反编译并还原其参数选择、触摸区约束和边界分支。
第一版目标是移植已验证的纯定点局部公式，而不是复制函数名。

当前 `edge_boost_pct + edge_push_q8` 同时改变信号和坐标，可能放大角落噪声。新求解器应能
分别开关“检测增益”和“坐标修正”，避免两个变量一起调。

完成标准：raw fixture 中坐标不越区、不跨峰；静止、直线和圆拟合残差下降；边缘系统手势
的首点更靠近物理边缘；快速滑动不增加一帧 down 延迟。

### 阶段 4：实现真正可计时的速度自适应坐标滤波

空间坐标稳定后再处理时间域。不要继续把当前 `euro_*` 直接称为标准 1-Euro。建议新增
明确的 filter mode，并保持旧 sysfs 参数兼容：

- `off`：游戏/延迟基线；
- `legacy_speed_ema`：当前行为，作为回退；
- `one_euro_fixed`：使用真实帧间隔、低通后的导数、`min_cutoff/beta/d_cutoff`；
- silent gap、resume、slot 重建和方向突变时重置导数历史；
- X/Y 独立状态，所有计算使用有界定点数，禁止在内核使用浮点；
- 滤波只处理已匹配坐标，不参与触点存在性、peak ownership 或 UP 判定。

完成标准：日常 profile 的圆/斜线观感和量化残差改善；高速跟随误差、down/up 时刻、slot ID
不退化；游戏 profile 默认关闭或采用经测量确认的低延迟参数。

### 阶段 5：按原厂思路扩展异常帧分类

在现有 line/area noise hold 上，以 diagnostics-only 模式依次加入：

- common-mode 与 RX/TX line noise 分离；
- charger noise；
- signal disparity；
- raw jump、长期负信号和 mutual/finger 状态不一致；
- compact real touch 与面状/整线干扰的空间区分。

每个分类先只记录 confusion matrix 和现场样本。只有证明确认率足够高，才允许该分类
阻断求解并保持上一触点；持续异常再交给阶段 1 的恢复状态机。真实快速滑动不能因为
raw jump 或线性轨迹而被误判。

完成标准：已知噪声不污染 baseline、不制造 UP/鬼触；正常快滑、双指和边缘触控误报为零。

### 阶段 6：只按现场证据补 tracking 缺口

当前 tracking 已不是主要短板。只有 trace 明确出现下列情况才继续：

- gap 后多个候选争抢同一 slot；
- 双指交叉时 ID swap；
- 快速移动超过现有预测 gate；
- established peak 与 split child 的 ownership 再次翻转。

候选改动包括 mutual-best/ambiguity-aware GapRelink、基于时间的速度与加速度 cost，以及
更明确的一组竞争峰单所有者模型。peak ID 只能是代价证据，不能成为硬锁；不得用全局增加
debounce 或 lost frames 掩盖问题。

完成标准：单指 split 全程一个 slot；双指交叉保持两个 ID；gap 恢复不连接到歧义候选；
首触和抬手时序不变。

### 阶段 7：有证据时再扩展 SafeBaseline/BLReset

原厂 SafeBaseline queue 容量为 5 已有直接反编译证据，但当前固定平板未必需要相同复杂度。
只有日志证明存在两个以上可重复、稳定且空间形态不同的显示/充电电气状态时，才扩展：

- 多个已提交 safe candidate；
- 明确记录创建时间、已确认的硬件状态和质量统计；
- 有手指时只选已提交 candidate，禁止学习触摸区域；
- 选择失败回到 wake qualification，不直接覆盖 working baseline；
- LRU/质量淘汰和完整拒绝原因。

在反编译字段语义未确认前，不应假定原厂队列项一定绑定姿态、频点或折叠状态。先从实际
调用和结构写入点还原，再决定 Linux metadata。

完成标准：不同电气状态可稳定复现并选中正确 candidate；错误选择可回退且不会产生鬼触。

### 阶段 8：最后评估扫描率和动态跳频

原厂架构由 TSACore 选择 freq/rate index，再由 AFE 邮箱执行。Linux 当前只 best-effort 请求
120 Hz，尚未验证命令完成状态。进入本阶段前必须：

- 实现 clear-status → command → done/status polling → timeout 的完整握手；
- 证明固件暴露的频率/扫描率表及 index 含义；
- 若频率改变 raw 分布，为每频点重新 qualification 或维护独立 baseline；
- 设置最小驻留时间、切换次数限制和失败回退；
- 触摸中切换不能释放 slot 或形成异常采样间隔。

只有现场日志证明某频点存在持续干扰，动态 hopping 才有收益。它不是当前平滑度或普通断触
的前置条件。

## 6. 明确暂不移植

- 完整 ApDaemon/TSACore 用户态框架和字符串命令总线；
- HPP2/HPP3 主动笔、蓝牙笔、ASA 校准和压力映射；
- 工厂 open/short/raw/noise inspect 与 firmware flashing；
- 折叠屏、侧边音量/电源键、proximity 和手机通话状态机；
- 完整 AFT 防边缘误触状态机；它可能降低屏外起始系统手势成功率；
- DoubleReport/Bezier 插帧；它会改变事件时序和游戏延迟；
- 内核 gesture/long-press 状态机；Linux input/Android 上层已经拥有事件语义；
- 未等待 done 状态的自动 calibration；
- 没有现场证据支持的多频 baseline 和自动跳频。

## 7. 每阶段交付约束

每一阶段必须独立提交，并满足：

1. 保留运行时回退或独立 profile，不把实验行为直接变成唯一默认路径；
2. 增加可重放 raw fixture 和对应 host regression；
3. 执行 `make check`、普通模块构建和 `HIMAX_DIAGNOSTICS=1` 构建；
4. `git diff --check` 通过，普通构建不意外带入完整诊断 ring；
5. 诊断字段区分“未触发、触发并保持、触发恢复成功、恢复失败”；
6. 先在 Linux 诊断版验证，再同步 Android 内核；同一次 A/B 只改变一个子系统；
7. 覆盖连续亮灭屏、四边快滑、多指交叉、静止/直线/圆、充电噪声和游戏 profile；
8. 任何 wake 稳定性、slot 连续性、边缘手势或延迟退化都回退到阶段 0 基线。

## 8. 下一步的实际选择

若目标是继续提高可靠性，先做阶段 1；若当前实机已确认亮屏和断触稳定、主要问题变成
“画线不圆、坐标抖”，则阶段 0 采集基线后并行准备阶段 2，但实际改变触控输出仍按
阶段 2 → 3 → 4 的顺序进行。

不建议下一步继续泛化 tracking，也不建议立刻复制整套 TSACore。当前最有价值的闭环是：

```text
可证明帧流健康
  -> 识别真实面板 profile
  -> 改善空间坐标求解
  -> 最后用正确的时间滤波处理剩余抖动
```
