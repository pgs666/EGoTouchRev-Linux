# HX83121A V4L2 用户态触控架构

2026-10-22：本分支采用“内核传输、用户态 TSACore、uinput 上报”。本文替代此前的
misc 字符设备/结果回交/运行时内核 fallback 草案。当前实现与旧内核算法以构建选项
互斥，不在运行中自动切换两个求解器。

## 数据边界

```text
SPI IRQ
  → threaded IRQ 一次 SPI 事务读取 5060 字节 master（线上含前缀为 5063 字节）
  → vb2 META_CAPTURE（HXST，MMAP）
  → 用户态校验 native master、前补 3 字节 THP 头
  → 原版 HimaxAfeAdapter + TSACore + TsaOutputMapper
  → uinput Type-B snapshot
  → Linux input/libinput
```

内核包含 `himax-v4l2.c`、共享的 `himax-spi-hw.c` 和可单独 host 测试的协议
`hx-otp.c` / `hx-afe.c` / `hx-raw-init.c` / `hx-ahb.c`。不链接 baseline、
peak detection、tracker、滤波、input 或旧诊断 ring。保留的硬件职责包括 GPIO reset、
AHB/SPI 事务、OTP 读取、reload、raw producer、panel follower、系统 PM、充电状态和命令邮箱。
这些必须由硬件拥有者串行执行，无法可靠地简化为毫无生命周期的 SPI 字节转发器。

用户态通过 submodule 编译原版全部启用算法，保留上游模块边界与面板参数表。
不把 C++ 浮点和进程级全局状态搬进内核。手写笔采集未启用。
这里的“原版”是上游 EGoTouchRev v1.4.0 的 C++ 重写实现，不能据此声称直接运行
华为原厂 DLL，或已经通过零售驱动的逐帧等价验证。
Linux 有两处宿主方向适配：构建时生成 `TsaCoreSolver.cpp` 与 `Sys.cpp` 的副本，
分别在 `ApplyPenSession()` 后和 `screenRotation = 0` 后应用宿主方向。
另生成 `TouchReport.cpp` 与 `Contour.cpp` 的副本：前者调用本地的首次获准上报
边缘起点修正，后者修复插值最后一行/列的越界取样。
`Tsa.cpp` 副本在峰检测前观察有界的边缘信号历史，补偿首个候选已经越过窄 ER 带的
快速滑入；`Sys.cpp` 在算法复位时清除此历史。
[`PrepareUpstream.cmake`](../userspace/cmake/PrepareUpstream.cmake) 校验五份原始源文件
的完整 SHA-256；源文件变化就停止配置并要求审查。submodule 本身不被修改，
其余算法源文件、接口布局和参数表不变，不能把本地修正称为上游等价行为。
边缘修正的条件、限制和对照工具见
[`userspace/README.md`](../userspace/README.md#边缘滑入的首报修正)。

## V4L2 ABI v2

源定义以 [`hx-v4l2-uapi.h`](../touchscreen-hx83121a-dkms/hx-v4l2-uapi.h) 为准。
所有结构使用固定宽度字段、显式版本/长度和零保留字段，64 位字段显式对齐。

可选用户态运行窗口采集不新增内核 ioctl，不另开 V4L2 consumer，也不 reset AFE。
默认无录制管理线程/队列；显式配置后由 SIGUSR2 触发限时窗口。其 HXRC raw 格式
不变，但侧车标注 `HXSE 2 live-no-checkpoint`，严格 session replay 拒绝中途状态；
fresh 录制现写 HXSE 3 recovery（继续读取旧 HXSE 1）。配置、文件和验证边界见
[运行中窗口录制](../userspace/README.md#不重置当前触控的运行中窗口录制)。
`c00844e` 已在本机完成两个无重置窗口（560/2 帧）的首轮验证，并安装用户态程序；
测试配置已移除，默认仍关闭录制。尚无有触点录制的实机时延/手势质量验收。

| 接口 | 内容 |
|---|---|
| `V4L2_BUF_TYPE_META_CAPTURE` / `HXST` | 一个 buffer 对应一个 5184 字节 `hx_raw_frame` |
| payload | 容量仍为 5132 字节；`payload_size=5060` 表示完整 master，剩余 72 字节必须为零；不在内核解析 suffix/checksum |
| epoch/sequence | 每次 AFE 重启/屏幕停止改变 epoch；成功读取后 sequence 递增 |
| timestamp_ns | IRQ 线程入口或主动补读开始时的 CLOCK_MONOTONIC 时间，不是硬件采样时间 |
| flags | AFE SOS 恢复原因、当前充电状态；协商后的主动读取带 `HX_RAW_FRAME_REARM` |
| `VIDIOC_HIMAX_INFO` | ABI、网格 40×60、payload 长度、OTP project ID、固件坐标变换 |
| `VIDIOC_HIMAX_COMMAND` | ClearStatus、频点、扫描率、IDLE、calibration、reset |
| `VIDIOC_HIMAX_REARM_TAGGED` | 带明确来源的主动补读；旧模块不支持时返回 ENOTTY |
| `VIDIOC_HIMAX_RESTART` | 精确 epoch/sequence 的静默 producer 异步重建，允许首帧未到；受理不等于恢复 |
| `V4L2_EVENT_HIMAX_STATE` | running/epoch；即使屏灭不再产帧也能及时释放触点 |

AFE 初始化重试（已部署普通模块）：STREAMON 或恢复 work 每次只尝试一次，失败后发布
stopped epoch、保持 IRQ 关闭，并按 Windows 200→500→1000→1500→2000 ms 异步
退避；不持 op_lock 等待重试，也不直接把初始化错误转换为服务重启。STREAMON=0
不代表硬件 ready，必须等待 running/raw；移除设备仍通过队列错误断开。
迟到 work 必须通过电源/队列门控；真实故障、休眠和工作队列并发仍待实机验收。
诊断版的 pre-SPI 主动失败已在本机验证 STREAMOFF 取消后不再尝试，以及后续新流
无需重启消费进程即可恢复；它不是物理 SPI 错误注入。普通模块无该测试参数/分支，
运行与测试启动项 initramfs 已同步，更新后的镜像仍待重启验证。

2026-10-23 起，新代码按固定 Windows `Frame::kMasterFrameSize` 一次读取 master，
不再为旧记录容量的尾部发出第二次 event-stack 命令。结构大小和 ioctl 编号不变，
INFO 与每帧的 `payload_size` 表示实际读取长度；新用户态兼容旧模块/文件的 5132
字节长度与新模块的 5060 字节长度，并拒绝新格式非零 padding。旧用户态会拒绝新
模块，必须配套更新 daemon，不能称为双向兼容。`44e6917` 已在
`7.1.8-gaokun3-himax1` 实机部署并更新测试启动项 initramfs；短期采集通过，
真实手势质量、睡眠与长期验收仍未完成。

命令必须来自当前 vb2 queue owner，并与当前 epoch、最近八帧内的 sequence 关联。
校验 action、参数和保留字段；普通 action 最短间隔 20 ms，reset 最短一秒，
重建 AFE epoch 不清除 reset 的限频时间戳。
这是邮箱传输确认，calibration-done 仍通过后续帧状态确认，不能把写入成功称为校准完成。

另外增加 ABI v2 的独立 `VIDIOC_HIMAX_REARM`（现有 ioctl 和结构布局不变）。
它不是邮箱 action：`hx_raw_rearm` 为 32 字节，要求当前 queue owner、运行态、
完全相同的 epoch/sequence（首次帧前允许 sequence=0）、200 ms 生产静默、
200 ms 请求限频及可用的 vb2 buffer。任何保留字段必须为零。
请求和 IRQ 复用同一完整帧发布函数；IRQ 抢先产生新帧时返回 ESTALE，补读抢先时
忽略比补读时间戳更早的已排队 IRQ，避免它再读一次同一事件。不会切换 epoch、
通知 AFE stopped/started 或重置 solver。成功仅代表完整 SPI 传输并交付到 vb2，
不代表 checksum 有效，更不代表触控故障已修复。无 buffer 返回 ENOBUFS 且不读 SPI。
新接口及用户态策略已编译/离线测试，曾在 `7.1.8-gaokun3-himax1` 部署普通模块
与修正后的 daemon。后续有触摸报告的实机运行出现五个空补读触发 reset，曾将
daemon 回退到 `c00844e`。随后来源标记/no-data 修正已随 `1674f8e` 配套安装，
但仍默认关闭自动补读、继续四秒等待。下文新恢复策略仍是实验代码；
最新时序与风险边界见 SPB 后端审计末节。
HEAD 现要求显式 `--experimental-rearm` 才自动使用补读；不带该选项时仍采用四秒
停帧等待，普通 service unit 不添加实验参数。实验模式拒绝与 `--info`/`--observe`
组合。接口及代码保留，后续在来源区分、空包和过渡时序验证后再评估默认启用。

邮箱协议的生产实现位于 `hx-afe.c`：写入 16 字节包体，再写四字节触发头，最后读回
16 字节；只有三步均成功才轮转五个槽位，任意失败保留当前槽位供重试。host 测试会
直接编译固定版本 Windows `HimaxProtocol.cpp` 中的原始构包函数，对全部 65,536 种
命令/参数组合逐字节比较，并对五个槽位的三步事务分别注入失败。原始构包函数的提取
仅用于测试，不加入运行时求解器或内核，也不会把读回解释成固件完成回执。

`scripts/test-v4l2-afe.sh` 是另行执行的真实固件检查，不属于 CTest。它保存原始帧，
区分传输成功和后续 suffix 完成状态；`--basic` 明确排除频点检查，不能当成全套验收。
2026-10-23 本机校准、清状态、IDLE 已观察到完成状态，但频点 1 命令后，五秒内
`word[10]` 和 `word[2]` 未变化，跳频仍未验收。当前上游 caps 的 `freqHopMode=0`，
手指默认管线不主动请求手动跳频；不得为通过测试就擅自改为 host 跳频。
临时模块试验可用 `HIMAX_TRIAL_AFE=basic|full|frequency` 配合 `--trial`，只在此时
启用独立的邮箱诊断编译选项，记录现有 16 字节读回；默认普通模块不包含日志或参数。
隔离实验在不再显式校准、稳定等待两秒后仍未见手动切频完成；命令参数写入已由
读回确认，但尚无 Windows 同命令序列对照，不应直接称为移植回归。

ABI v2 将 INFO 的原保留字段定义为 `coordinate_flags`，结构仍为 40 字节。
bit 0/1/2 分别是 invert X、invert Y 和 swap XY；其他位必须为零。内核从固件
touchscreen 属性读取，用户态先翻转再交换轴，然后声明对应的 uinput 量程。
本机是 `5`，uinput 坐标量程为 `15999×25599`；solver 和回放 CSV 仍在原始
`2560×1600` 面板方向工作。compositor 的桌面旋转与这项接线变换分开，但 live daemon
通过 `SolverScreenState` 将宿主方向传入 `Core::SetScreenRotateState()`。方向来源优先级为：
`--rotation` 固定值、显式 `--rotation-file`/`HIMAX_SCREEN_ROTATION_FILE`、KDE 桥接消息。
不读取 `fb0/rotate`：它与 compositor 方向不同。本机 framebuffer 为 0，而 KScreen 为 8。
live daemon 要求 ABI v2，不与 ABI v1 模块混用。

独立的 `himax-screen-bridge.service` 在桌面用户会话每秒查询一次 `kscreen-doctor --json`，
只读取已启用且已连接的指定内屏（默认 `DSI-1`）。`Output::Rotation` 的
`None/Left/Inverted/Right = 1/2/4/8` 转为 TSACore 的 `0/1/2/3`，拒绝镜像模式。
枚举与 Wayland 映射依据 KDE libkscreen 的 `src/output.h` 和
`backends/kwayland/waylandoutputdevice.cpp`（本机另有 `kscreen-doctor -j` 实测）。
桥接发往抽象 Unix datagram 地址 `himax-touchd-screen-state`；接收方启用 `SO_PASSCRED`，
只接受 root 或 `sd_seat_get_active("seat0")` 对应 UID，消息限长 64 字节，每 250 ms
至多消费 16 条。查询不进入求解循环，不访问主服务被隔离的桌面 D-Bus/home。
桥接后台线程每秒查询并重发；请求接收线程与最长两秒的 KDE 查询分离。成功查询缓存
最多复用三秒，查询失败/面板缺失时清缓存，接收端未运行导致发送失败则保留缓存。
缓存回复与周期发布通过同一锁串行，避免新方向已经发送后又发出旧缓存。

daemon 在打开设备和 STREAMON 前建立接收器，并向抽象地址
`himax-screen-bridge-<seat0 UID>` 发送一次非阻塞 `HXSR 1\n` 请求；
没有活动 seat 时只尝试自身 UID。桥接只接受 root/自身 UID 的真实内核凭据，
且请求源地址必须恰为 `himax-touchd-screen-state`，回复也固定发到该地址。
请求严格检查版本/长度；桥接每批最多 16 条，批次后等待 250 ms，请求不触发新查询。
两端都拒绝截断报文、意外 ancillary 数据和文件描述符，并关闭已接收的 fd。
显式固定方向或文件源不发送初值请求。daemon 不增加线程或等待应答，
主服务的隔离权限保持不变。

握手利用 AFE 初始化时间接收近期缓存，减少重启后首条周期消息前的方向空窗；
不保证桥接缺失、缓存过期或应答较迟时第一帧就有宿主方向。无新来源时保留最近方向，
首次无来源时使用上游默认值，周期广播负责后续收敛。真实首帧和失败回退结果见
[`userspace/README.md`](../userspace/README.md) 的增量验证记录。

方向变化不重置 AFE/control/mapper/baseline，不强制触点抬起，也不重复变换上报坐标。
`SolverScreenState::Process()` 建立仅限当前线程、当前求解调用的方向作用域；
求解器初始化及笔默认状态应用后，通过上述宿主入口设置实际方向。`MsReset` 在当帧内
继续执行检测与握持流程，因此 `Sys::Init` 清零方向时就立即应用宿主值，不能等
`Process` 返回才补救。没有方向来源（`-1`）时两处入口均不覆盖上游默认行为。
线程局部状态不在不同线程、嵌套作用域或后续无方向的会话间泄漏，不增加等待或 I/O。
手写笔状态不会被用作传送显示方向的假通道。

四种初始方向及校准/IDLE/AFE epoch 的 host tests 检查实际 Core 状态；
另直接调用生产的 `Sys::Init`、`MsReset` 和 `GripFilter::RegionProcess`，确认
复位内部的方向和随后生成的握持区域与显式宿主方向一致。构建测试检查上游漂移/
缺失文件被拒绝、submodule 源文件未修改、无变化配置不会强制重编。
实际四方向边缘手势和握持手感仍须实机验收。原始 HXRC v1 不保存方向变化，固定方向回放可使用
`himax-replay --rotation 0..3`；带 HXSE 侧车的新录制使用 `--session` 重放每帧实际方向。

`--info` 使用只读 fd 查询 QUERYCAP/INFO，不分配 buffer、不启动 AFE、不申请
队列所有权，可以在常驻服务运行时使用。本接口是项目私有的 V4L2 metadata 格式，
不是标准 V4L2 touch 矩阵格式，通用 touch 工具不能直接解析 HXST event stack。

### 录制容器

录制文件在 HXST 帧之前保存 32 字节 `HXRC` 头。整数均为 little-endian：

| 偏移 | 长度 | 内容 |
|---|---|---|
| 0 | 4 | ASCII `HXRC` |
| 4 | 2 | 容器版本 `1` |
| 6 | 2 | 头长度 `32` |
| 8 | 4 | 后续帧长度 `5184` |
| 12 | 16 | NUL 补齐的 project ID |
| 28 | 4 | `coordinate_flags` |

头在实际 AFE 身份确定后写出一次，随后是未经修改的 `hx_raw_frame`。
回放优先使用记录的 project ID；显式覆盖会输出提示。仍可读取旧的无头 ABI v1/v2
录制，旧文件应由操作者提供正确面板身份。容器头不改变 live HXST buffer 格式。

### 正常 AFE 闭环录制

`--record PATH` 在正常输入或观察模式均可使用，另建 `PATH.events`，不改变 HXRC/HXST
格式。两文件以独占创建方式打开。录制初始化在输入循环之前完成；启用时分配 128
个事件的 SPSC 队列，生产者只做有界复制/原子操作和 nonblocking eventfd 通知。
后台线程写 raw 和侧车；队列满或写入失败后停止接受新证据，输入保持运行。
退出先释放输入，再排空队列；异常退出、丢事件或缺少尾记录均不能通过严格回放。
未启用录制时没有队列、写盘线程、raw 诊断复制或输出摘要计算。

fresh HXSE v3 为 ASCII、十进制整数、每行一条记录，解析限长 1023 字节：

```text
HXSE 3 recovery
B PROJECT_ID coordinate_flags observe experimental_restart
S host_ns epoch running
R host_ns
J decision_ns completed_ns epoch sequence errno
F index host_ns rotation report afe_status ctrl contact_digest action_count [action parameter result]...
END frame_count dropped errno
```

`B` 只出现一次；身份尚未确定前可先出现 `S`。每个 `F` 对应 raw 中一个完整 HXST
包（仅记录进入求解/原始帧校验路径的 accepted 帧），保留原包的 epoch、sequence 和
IRQ 时间戳；被 transport 时序策略丢弃的旧帧不进入求解或录制。`S` 是主线程实际处理
的有效生命周期事件；`R` 是无新帧 250 ms 的输入释放，不复位算法 mapper。
`F` 的方向为 TSACore 的 `-1/0..3`；`-1` 表示宿主尚未获得方向。
`J` 是无新帧时的独立重建请求，不伪造 raw 包或 frame-bound action。
记录 policy 决策时间、ioctl 完成时间、精确 producer token 和返回 errno；0 只表示
scheduled。只有 `B` 明确开启实验恢复且非 observe 时才接受；失败请求不清 solver，
受理请求按 `OnAfeSos → OnAfeStopped` 隔离旧流，后续新 epoch 重新 `OnAfeStarted`。
生命周期、ready event、SPI 完整包和触控质量恢复仍是不同层次的证据。

继续支持旧 `HXSE 1`：其 `B` 没有实验标志、也不允许 `J`。新 writer 总是使用 v3，
旧 reader 不能假装理解新恢复流程。运行中窗口仍用 v2/no-checkpoint，可能包含
`J` 供人工诊断，但严格 replay 继续拒绝它，而不推测中途 solver 状态。

`action` 使用 `hx_raw_action` ABI，`parameter` 为原始参数，`result=0` 表示命令
传输成功、正数为 Linux errno、`-1` 表示观察模式没有执行。后续 raw 状态仍是固件
完成的证据。触点摘要为 FNV-1a 64：先送入触点数量，再按 mapper 顺序送入每点的
id、x/y 的 IEEE float32 位、state、isReported、reportEvent、signalSum；各值均按
32-bit little-endian 展开，不包含 padding/地址。

`--session` 同时检查 raw 与侧车、面板/方向/生命周期、命令请求和执行反馈、逐帧
report/status/ctrl/触点摘要。只有收到 `END`、两文件数量匹配、dropped/errno 为零且
至少有一帧实际求解成功，才报告 `live_output_verified=1`。正常采集包含完整初始化
历史；它不是既有进程的中途状态 checkpoint，也不能把跨编译器浮点差异当作同一输出。
录制没有逐帧 fsync，完整性检查不承诺掉电持久性。
重建回放验证请求 token、结果范围/时间、重试与等待预算、旧流隔离和算法输出。
它不重放实际 SPI，也不独立证明请求前的 kernel 静默门控或 producer retry 预算
为何耗尽；这些由生产内核门控/host policy 测试及真实故障采集分别提供证据。

## 队列和时序

当前源码由分配回调固定返回两个 buffer，daemon 也只申请两个，匹配已审计
`SpbThpTool.sys` 的两帧 FIFO 容量和队满丢新策略。此前版本是 2～8 个、daemon
默认四个。没有可用 buffer 时仍排空 SPI，丢弃新交付；旧两帧的内容/时间戳不被
覆盖，producer sequence 继续增加，后续接收能看见间隔。它不是 latest-only 队列，
也不能声称每帧保留或不会丢 UP。用户态取回后立即复制并 requeue，不持有映射
等待算法。容量只限制保留帧数，不给调度延迟设上界。

不能直接设置 `vb2_queue.max_num_buffers=2`：当前内核要求这个索引空间上限至少为
`VB2_MAX_FRAME`，否则 WARN；实际容量通过 queue_setup/REQBUFS 协商。
本驱动没有提供 CREATE_BUFS 扩容 ioctl。两帧包括正在被用户态复制的 buffer，
与 Windows 动态节点 FIFO/异步 completion 的调度并非逐项等价；仍需实测。
该两帧策略已随异步启动清帧安装到当前 Linux 测试内核，启动镜像也已同步；
冷启动与长期触控负载仍未验收。具体版本与备份见用户态运行说明。

硬件不等待算法线程。MMAP 避免 read 的 copy_to_user，但当前仍有
SPI bounce buffer → event buffer → vb2 buffer → 用户态私有帧的拷贝，不声称零拷贝。
约 0.6 MB/s 原始数据带宽的瓶颈应通过测量判断。

daemon 使用 poll，逐帧求解；超过 50 ms 的旧帧、重复/乱序 sequence 和过期 epoch
不能上报。生命周期事件和 vb2 帧来自两个独立队列，必须先处理事件，再校验帧 epoch。
屏灭后的旧完成帧不能重新激活 input slot；此行为由纯状态回归测试覆盖。
只有原版 solver 请求上报时才提交 Type-B snapshot；无触点时不持续写空输入帧，
跳过上报的帧也不能被误解为一次全抬起。

正常链路不增加固定轮询等待和额外触控防抖，但增加了进程调度和 uinput 开销。
输出的 IRQ→uinput 分位数涵盖 SPI 读取、调度、算法和上报；不代表物理点击到应用
响应的总延迟。没有实测不能保证性能优于旧实现。

## 硬件和算法生命周期

当前源码与已部署测试模块的 STREAMON、亮屏恢复和受限 reset 执行如下；
正常启动、显式 reset、启动中 STREAMOFF 已试载验证，系统睡眠等仍有验收缺口：

```text
关 IRQ / 停止事件
→ epoch++
→ master 0x13 bus-ready 探测（零值等 500 ms，I/O 失败异步退避）
→ reset/detect（每次一次失败后按 200/500/1000/1500/2000 ms 异步重试）
→ 首次 OTP 读取后再次 reset/detect
→ reload / MCU ready / raw runtime
→ 开 IRQ producer，初始读取一次
→ 三次启动消费尝试：IRQ 数据优先，等 200 ms 超时才补读
→ StartCalibration / 120 Hz
→ running 事件 / 交付后续数据
```

OTP 使用原版独立的 AHB burst、DD access 解锁和只读槽位选择流程，
并在成功和失败路径恢复 DD/safe-mode 状态；控制触发寄存器不强求等值读回。
本机已从槽位 `0x0f18` 读出 `W273AS2700`，算法据此选择参数。
寄存器协议测试还覆盖部分读取失败、清理失败和逐次总线操作故障注入。
屏灭期间 STREAMON 不启动硬件，也没有可用 OTP。daemon 等首个运行 epoch 才重新
查询 INFO 和选择参数；同一会话内身份变化会退出，不允许在已有状态中静默换 profile。

收到新的 epoch 时，用户态释放旧 slot、通知 OnAfeStopped/OnAfeStarted；SOS 重启
额外通知 OnAfeSos。TSACore 对 calibration-done 上升沿、baseline reset、保留状态和
频点切换的处理直接沿用上游。不能把上游 profile 未启用的 SafeBaseline 五槽描述为
当前有效功能，也不能假设 reset 就能解决掌压亮屏。

系统 suspend/freeze/poweroff 与 panel 开关是独立输入。系统休眠先同步关闭 IRQ，
在 `op_lock` 下设置休眠状态、使 AFE 失效并发出 stopped/新 epoch，然后在锁外
取消初始化和充电任务。停止不发送 sense-off、不拉 reset，也不访问 SPI。
IRQ、AFE 启动、命令和充电任务共用 `hx-raw-policy.h` 的状态检查；休眠期间晚到的
panel enabled 不能重新启动硬件。resume/thaw/restore 在 SPI 父设备恢复后解除系统
休眠状态，仅在 streaming、panel prepared/enabled 全部满足时排队完整初始化。
因此两种 panel/system 恢复顺序都可工作，休眠期间 STREAMOFF 则不会重启。
shutdown/remove 先关闭入口再取消任务，不在持有 `op_lock` 时等待 worker。

充电变化通过 power_supply notifier 排队，在硬件互斥锁下更新 firmware。分别保存
物理连接状态和固件已确认应用的状态，前者随帧交给原版 adapter；没有在 IRQ 里查询
整套电源属性。启动时或通知后的写入失败，不会被误记为已应用；每秒重试电源查询/
写入，成功后停止重试，每次硬件初始化都会使已应用状态失效。屏灭、系统休眠、
STREAMOFF 和 shutdown 阻止后台硬件访问；新 AFE 启动时重新安排检查。

状态测试覆盖生产代码使用的全部 64 种 power flag 组合、两种恢复顺序和充电失败
状态；它不能替代真实系统 suspend/resume、物理拔插和 SPI 故障注入测试。

锁顺序为 video_lock → op_lock → buffer_lock。IRQ 不获取 video_lock；
硬件事务不持 buffer_lock。停止/解绑先同步 IRQ，再归还队列。IRQ 和 notifier/work
在释放状态前注销，V4L2 引用计数保持已打开文件和 mmap 所需状态。

## 故障与恢复

- SPI 半帧不会进入 vb2；连续三次 IRQ 读取失败停止 IRQ 并重启 AFE。
  主动无复位补读失败只返回错误，不能借这条错误路径直接触发 reset。
- 用户态连续五次 native 校验失败或遇到 SOS，可申请受限 reset。
- 无有效新帧 250 ms 后释放已上报触点。
- 停屏事件立即释放，不等待这个超时。
- 进程正常退出或被杀时关闭 uinput，虚拟输入设备销毁。
- systemd 三秒 watchdog 覆盖算法挂住的情况，停止/休眠期间也持续喂狗。
- 新代码先尝试无复位补读：正常模式等待 200 ms，IDLE 等待 18×200 ms；
  ioctl 完成后至少留 20 ms 的取帧机会，无进展时按 200 ms 等待加 20 ms grace 继续尝试。
  七次请求或每次请求前累计等待已超过 3200 ms 时，在该次请求的 grace 后耗尽预算，
  再退出让 supervisor 重建 stream/AFE。轮询调度会使实际时间向上取整。
  IDLE 的第一笔补读可能已超过累计预算，仍会发出并留出 grace，不跳过最后一次补读。
  这是单 master 的适配，不等于 Windows 双通道串行取帧的总耗时。
- `--observe` 不发主动补读；旧模块返回 ENOTTY 时，新 daemon 也恢复四秒超时路径。
  屏灭不计入停帧超时；重复 running 事件不能掩盖停帧。用户态分别统计请求、
  transfer 成功、竞态/门控拒绝、失败、不支持，以及之后实际收到有效帧的次数。
- 计数清除以有效 master 校验为依据，不能因收到 sequence 增长的全零包就清除
  累计 rearm 次数。普通无效 IRQ 包不沿用旧 IDLE 标志；明确标记的空轮询不更新
  已验证的 mode，也不推进 producer deadline/IRQ 错误计数。实机在正常 IDLE 静默期间主动补读
  确实返回过全零，后续正常 IRQ 继续有效出帧，不能把这种试验称为丢中断恢复。
- supervisor 等待两秒后持续重试，不因十次启动限制而永久停止硬件恢复。
- 多个消费者不能同时拥有采集队列；不允许内核和用户态同时上报同一块触屏。

服务退出后桌面触控暂时消失，由 systemd 重建服务；没有运行时内核求解 fallback。
保留旧实现的价值是显式恢复构建/部署，不是假装两套 baseline 可以无缝交接。
升级试用期间临时 mask 常驻服务，避免 udev 让旧 daemon 抢占新版队列。
卸载模块可能连带卸载依赖，部署和回退均在卸载后重新加载目标模块的依赖，
并在恢复后核实设备绑定及原服务状态。

新来源协商不改变 ABI v2 的结构尺寸：新增独立 `VIDIOC_HIMAX_REARM_TAGGED`
（private +3）。旧 `VIDIOC_HIMAX_REARM`（+2）保留旧 flags，不向旧调用者输出
其不认识的标志。新 host 只用 +3；旧模块即使支持 +2，也会以 ENOTTY 回退，
不会猜测 untagged 包来自主动补读。普通 IRQ 始终不带 REARM。

只有 `REARM + AllZero` 被解释为“轮询无数据”：不送入 solver、不更新 baseline、
不推进 IRQ 错误阈值，也不清已有真实错误；pending reset 不被它取消。
REARM 的 checksum、filler、长度错误仍按真实错误处理，有效 REARM 仍正常解算。
raw HXRC/HXST 与 fresh HXSE 1/3 侧车配对读取都保留来源；strict replay 用同一规则。
raw `--hxst` 单独列出 `empty_rearms`，不把它们称为有效帧；没有有效帧仍不能回放成功。
旧文件不含来源标记时保持原错误语义，不能根据时间接近就给它补造 provenance。

同进程重建目前需要显式 `--experimental-restart`，独立于 `--experimental-rearm`。
未开启时仍按四秒 stall deadline 退出给 supervisor；只开 restart 时在四秒耗尽后
调用 +4；两者都开时在已有 producer kick 预算耗尽后调用 +4。旧模块 ENOTTY 或
其他致命请求错误仍退出给 supervisor。该选项不是重新启用默认自动 kick。

重建受理立即停止 solver/input，并在事件队列尚未排空时就 fence 旧 epoch；
无效新帧/SOS 数据和 running event 均不能记为恢复。新 epoch 的 raw 与 AFE status
都可用才结束等待。Linux 额外设置竞态重试一秒、100 ms 退避、受理后十五秒等数据、
最多连续三次受理；连续三十秒好数据且无过长间隔后才恢复预算。它们是本地边界，
不是 Windows 原样参数。等待期间不嵌套发 frame-bound reset，不因更多坏帧无限
重置等待时间；既有 250 ms input-only release 保留。该路径尚未实机故障验收，
当前默认 unit 显式添加 `--experimental-restart`，仍不添加 `--experimental-rearm`；
带 live-capture 的可选 drop-in 和 fresh 采集脚本也保留相同恢复行为。
本机已配套安装模块/daemon，并同步测试内核的 initramfs；真实故障恢复和 PM/
手势验收仍未完成，受控 IDLE 重建不能代替它们。当前安装状态见 SPB 审计。

## Linux 与 Android

Linux 服务和 udev 安装、采集/回放及恢复命令见
[`userspace/README.md`](../userspace/README.md)。内核需要 uinput、video/vb2-vmalloc 和
当前 DRM panel follower 支持。

Android 还需要 native vendor daemon、init 生命周期、SELinux、uinput 设备映射、
旋转/显示关联和启动时序验证；现有诊断 APK 依赖旧 sysfs，不适用于新接口。
此次 Linux 实现不自动写 Android boot 槽位。

## 参考与验收

V4L2 metadata/vb2 和 Linux uinput 的本机源码/文档是接口依据。Google Touch Offload
的 REPORT 回交、IPTS/iptsd 的 uinput 都是可比较的架构，本项目采用后者的数据分工，
通过 V4L2 承载完整 raw event stack，而不是复制 Google 私有字符设备 ABI。

验收必须分开记录：

1. 内核与用户态构建、host tests、ASan/UBSan、真实旧 trace 回放。
2. 实际 V4L2 格式、MMAP/STREAMON/OFF、生命周期、命令闭环、uinput 枚举。
3. daemon 停止/挂起、屏灭/恢复、解绑时保留 fd/mmap 的安全性。
4. 快速边缘滑动、多指、掌压亮屏、充电切换和长期性能。

前面的项目通过不能替代后面的实机验证。
