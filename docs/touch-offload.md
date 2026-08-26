# HX83121A 触控算法 Offload 架构参考

本文记录将 HX83121A raw matrix 的复杂触控求解从 Linux 内核移到用户态的可行架构、
参考实现、接口约束和实施顺序。它用于未来设计评审，不表示当前驱动已经实现 offload，
也不要求为了追求架构一致性立即替换现有内核算法。

当前结论是：现有定点 baseline、检测、坐标和 tracking 算法仍适合作为可靠的内核默认
路径；如果后续目标扩展到完整 SafeBaseline/BLReset、多面板 profile、高级空间求解、
复杂滤波或快速算法迭代，则应优先考虑“内核采集与兜底、用户态求解、结果回交内核”的
双路径，而不是继续无限增加 threaded IRQ 中的算法复杂度。

## 1. 为什么考虑 offload

当前每帧包含 60×40 个 mutual raw 节点。按 16 bit 节点和 120 Hz 计算，纯矩阵数据约为：

```text
60 × 40 × 2 × 120 = 576000 byte/s
```

当前完整 event stack 约 5132 byte，120 Hz 时约 616 kB/s。这个带宽和数百万次每秒的整数
运算对现代 ARM64 并不大，因此 offload 的主要动机不是“CPU 算不动”，而是工程边界：

- 当前处理位于 `IRQF_ONESHOT` threaded IRQ，算法耗时会延长 IRQ 屏蔽窗口；
- 内核不能使用浮点和常规用户态数学/分析库；
- 动态内存、文件、配置、可视化和参数热更新都受到严格限制；
- 越界、死锁或未覆盖状态可能影响整个系统，而不只是触控服务；
- 算法更新需要重新构建、部署内核，Android 还涉及 boot slot；
- 大规模 raw trace 重放、A/B 和 profile 迭代在用户态更容易实现；
- 完整 Windows TSACore 级状态机的维护复杂度不适合继续堆在 IRQ 数据路径中。

另一方面，简单、确定、有界的矩阵遍历并不天然不适合内核。现有 2400 节点 baseline、
连通区和最多 10 点的 Hungarian assignment 仍可保留。是否迁移应由实测时延、可靠性和
开发目标决定，而不是仅按代码行数判断。

## 2. 可参考的公开实现

### 2.1 Google Android Touch Offload

Google 的 `kernel/google-modules/touch/common` 提供了最接近本项目目标的公开架构。本文
核对的参考分支为 `android-gs-tangorpro-5.10-android14-qpr3`，其接口版本为 2.0。

主要文件：

- [`include/uapi/input/touch_offload.h`](https://android.googlesource.com/kernel/google-modules/touch/common/+/refs/heads/android-gs-tangorpro-5.10-android14-qpr3/include/uapi/input/touch_offload.h)
- [`touch_offload.c`](https://android.googlesource.com/kernel/google-modules/touch/common/+/refs/heads/android-gs-tangorpro-5.10-android14-qpr3/touch_offload.c)
- [`goog_touch_interface.c`](https://android.googlesource.com/kernel/google-modules/touch/common/+/refs/heads/android-gs-tangorpro-5.10-android14-qpr3/goog_touch_interface.c)
- [`heatmap.c`](https://android.googlesource.com/kernel/google-modules/touch/common/+/refs/heads/android-gs-tangorpro-5.10-android14-qpr3/heatmap.c)

其 UAPI 支持：

- mutual/self 的 2D/1D 数据；
- raw、filtered、baseline、strength 和 firmware coordinate channel；
- frame index、纳秒时间戳和多个 channel；
- capability 查询和运行时配置；
- `START/STOP` 生命周期；
- 用户态通过 `REPORT` ioctl 把最终坐标交回内核；
- palm、grip、coordinate filter、continuous/auto reporting 等能力协商。

`touch_offload.c` 使用字符设备、预分配 frame pool、阻塞 `read/poll` 和 ioctl。它仍会在
读取时打包并 `copy_to_user`，所以不是零拷贝模板。`heatmap.c` 另行展示了基于 V4L2 vb2
的 `MMAP/USERPTR/DMABUF/READ` 矩阵输出路径，适合高吞吐诊断或未来零拷贝实验。

这套代码是 Google Android vendor module 接口，不是 Linux 主线通用触控 ABI。应借鉴其
边界和失败模型，不应未经审计整块复制到本项目。

### 2.2 Intel IPTS / iptsd

Surface 设备使用的 Intel Precise Touch and Stylus 生态采用另一种常见分工：内核负责
硬件传输，用户态 `iptsd` 处理 heatmap，并通过 `uinput` 产生输入事件。

- [`linux-surface/iptsd`](https://github.com/linux-surface/iptsd)
- [`linux-surface/intel-precise-touch`](https://github.com/linux-surface/intel-precise-touch)

它证明复杂 raw touch/stylus 求解可以作为 Linux 常驻服务运行。`uinput` 对桌面 Linux
原型简单，但 Android 上会增加 SELinux、设备枚举、启动时序、重复 input device 和服务
崩溃后的重建问题。本项目若实现统一的 Linux/Android 生产路径，更适合让用户态结果回交
现有内核驱动，再由同一个 `input_dev` 上报。

### 2.3 传统 Linux/Android 厂商驱动

Goodix、FocalTech、ST、Atmel、Himax 等公开驱动通常能读取 mutual raw，但正常输入路径
使用控制器固件已经求解出的坐标。raw 主要服务于工厂测试、ITO/open-short、min/max/gap
和 debugfs/sysfs 诊断。

Linux 的 `input_mt_assign_slots()` 只提供轻量的触点到 slot 最佳匹配，也不负责 baseline、
峰检测或坐标求解。因此很难在主线 `drivers/input/touchscreen/` 中找到与 Windows TSACore
等价的完整 raw matrix 求解器。这是架构选择的结果，不代表 Linux 禁止 host processing。

## 3. 本项目推荐架构

推荐保留两个互斥的数据路径：

```text
                         ┌─ kernel solver ────────────────┐
SPI IRQ → frame validate ┤                                ├→ one input_dev
                         └─ offload queue → native daemon ┘
                                             │
                                             └─ REPORT ioctl
```

### 3.1 内核职责

- SPI/AHB 访问、event-stack 完整读取和格式校验；
- reset、probe、panel suspend/resume、firmware reload 和 raw producer 握手；
- invalid/retry/SPI/停滞等运行时健康看门狗；
- 预分配 raw frame ring 和有界队列；
- 当前内核求解器，作为启动、恢复和 daemon 故障时的可靠 fallback；
- 严格校验用户态结果并通过唯一的 Type-B `input_dev` 上报；
- 管理 mode、epoch、frame deadline、丢帧和回退计数；
- 提供最少且版本化的 UAPI，不把任意寄存器访问暴露给算法服务。

### 3.2 用户态职责

- working/candidate/safe baseline 及更完整的 BLReset/BLSM；
- panel project ID 对应的参数 profile；
- peak、zone、空间亚像素坐标和四边独立修正；
- tracking、palm、异常噪声分类和时间滤波；
- 参数版本、热更新、实验 profile 和只读状态导出；
- raw trace 录制、离线重放、量化指标和算法 A/B；
- 在 deadline 内返回与输入 frame/epoch 严格对应的坐标。

Android 生产实现应是由 init 管理的 native vendor daemon，并配置最小 SELinux 权限。APK
只适合作为控制/诊断 UI；Zygisk、root shell 和常驻 APK 都不应成为生产输入链路依赖。
Linux 桌面原型可以由 systemd 管理同一个 daemon。

## 4. 数据与控制接口草案

第一版建议使用字符设备，例如 `/dev/himax-touch-offload`，使用 `read/poll + ioctl`。
先验证闭环，不要一开始引入共享内存或 V4L2 的全部复杂度。

### 4.1 能力

只读 capability 至少包括：

```c
struct hx_offload_caps {
    __u16 abi_major;
    __u16 abi_minor;
    __u16 rows;
    __u16 cols;
    __u16 max_contacts;
    __u16 frame_rate_hint;
    __u32 supported_channels;
    __u32 supported_features;
};
```

ABI 使用固定宽度类型，所有结构包含显式大小、版本和保留字段。禁止向 UAPI 暴露内核
指针、未填充的 padding、`bool`、`long` 或随架构变化的枚举宽度。

### 4.2 输入帧

每帧至少包含：

- `abi_size`：本结构的实际大小；
- `epoch`：每次 probe/full reset/resume producer restart 后递增；
- `frame_index`：同一 epoch 内单调递增；
- `timestamp_ns`：尽量接近 IRQ/硬件帧产生时刻；
- master status、retry、firmware finger state；
- rows/cols、channel mask 和 payload 长度；
- raw matrix；
- 可选 firmware coordinate、baseline/filtered matrix 和轻量 driver status。

`epoch + frame_index` 是结果关联的唯一身份。时间戳不能代替序号，因为 suspend、时钟精度
和批处理可能产生歧义。

### 4.3 输出结果

用户态报告至少包含：

- 对应的 `epoch + frame_index`；
- 结果生成时间戳和算法 profile/version；
- contact count；
- 每点的 stable ID、X/Y、pressure/major 和明确的 active/cancel 语义；
- 可选的质量/异常状态位，不允许用户态直接请求 reset 或任意寄存器写入。

内核必须验证：

- ABI 版本、结构大小和所有 reserved 字段；
- epoch/index 是否为当前仍可接受的帧；
- contact 数不超过上限；
- stable ID 唯一且在范围内；
- 坐标、pressure/major 和所有乘法/长度不会溢出；
- 过期、重复、乱序结果不能改变当前 slot；
- daemon 不能伪造 kernel lifecycle 或跳过强制 release。

## 5. 实时性、队列和拷贝

120 Hz 的帧周期约为 8.33 ms。用户态链路的目标不是“绝不丢帧”，而是避免排队形成不断
增长的输入延迟：

- 启动时预分配 4～8 个 frame buffer，IRQ 中禁止分配；
- producer 永不等待用户态完成复杂算法；
- 用户态落后时丢弃最旧的未处理帧，保留最新状态；
- 每个丢帧原因分别计数：无空 buffer、用户态超时、结果过期、epoch 不匹配；
- daemon 应使用 `poll/epoll`，避免固定周期轮询；
- 是否提高调度优先级必须基于 trace 测量，并设置 CPU/实时资源边界；
- 不应在每帧做日志格式化、文件 I/O、参数解析或堆分配；
- 第一版允许一次 `copy_to_user` 和一次小型 result `copy_from_user`；只有 profiling 证明拷贝
  是瓶颈后，才评估 mmap/V4L2 vb2/DMABUF。

必须分别测量 IRQ timestamp、daemon read、solver start/end、report ioctl 和 input report，
以分位数而不是平均值评估端到端时延。不能仅用 CPU 占用推断触控延迟。

## 6. 模式切换和 fallback

建议内部状态至少包括：

```text
KERNEL
  → OFFLOAD_ARMING
  → OFFLOAD
  → FALLBACK_RECONCILE
  → KERNEL
```

规则：

1. probe、boot 和 daemon 未连接时始终使用内核路径；
2. daemon 完成 capability/config/start 握手后，从新的 epoch 或明确帧边界开始 offload；
3. 同一物理帧只能由一条路径上报，禁止两个 input device 同时产生触控；
4. 切换时保守地 release 所有 slot，第一版不尝试跨求解器继承 tracking ID；
5. daemon close、进程死亡、结果连续超时、ABI 错误或队列长期拥塞时自动回到内核路径；
6. fallback 必须有退避，不能在 daemon 反复崩溃时形成 mode/reset storm；
7. suspend 会停止队列并使旧 epoch 失效；resume 完成硬件 qualification 后重新握手；
8. 硬件健康恢复始终由内核决定，用户态只能报告观察结果或请求受限的重新评估。

切回内核算法时 baseline 状态是关键问题。第一版应让内核求解器在 offload 期间继续以低成本
影子更新可信 baseline，或在 fallback 时先执行短 qualification；不能直接用数秒前冻结的
working baseline 上报。

## 7. Android 与 Linux 集成

### Android

- daemon 放在 vendor 分区，由 `init*.rc` 管理并设置明确的 restart policy；
- SELinux 只允许该域打开 offload 节点和必要的只读配置，禁止通用 sysfs/AHB 访问；
- 内核仍注册原来的 input device，InputReader 无需识别第二个虚拟设备；
- APK 通过受控 Binder/Unix socket 读取状态或切 profile，不能直接持有 offload fd；
- recovery、早期启动和 daemon 不兼容时自动使用内核 fallback；
- Android A/B 内核和 vendor daemon 必须进行 ABI compatibility 检查，major 不兼容则拒绝
  offload，而不是尝试猜测结构布局。

### Linux

- 相同 daemon 可由 systemd 管理；
- 开发早期可以用 `uinput` 快速验证算法，但这不是推荐的最终共用架构；
- 最终仍建议结果回交内核，从而复用同一个 input device、slot 状态和 fallback；
- daemon 缺失不能影响键盘、显示、suspend 或基本触控可用性。

## 8. 诊断、安全和隐私

raw touch heatmap 可以反映手掌形状、手写轨迹和用户操作时间，因此设备节点不能对普通应用
开放。需要：

- 单客户端独占打开；
- 权限和 SELinux 双重限制；
- 对所有 ioctl 使用 `copy_from_user/copy_to_user` 后再验证；
- 禁止任意物理地址、寄存器和固件刷写接口；
- trace 录制默认关闭，文件有容量/保留时间限制；
- production build 不输出逐帧内核日志；
- diagnostics 能显示 mode、daemon PID/连接代次、队列水位、丢帧、超时和 fallback 原因；
- 对恶意或卡死客户端进行故障注入，确保不能阻塞 IRQ、suspend 或 remove。

## 9. 分阶段实施建议

### 阶段 A：用户态复用现有算法，不改变实机输入

先把 `hx-algo` 的纯算法核心进一步整理成 kernel/host 可共用的固定宽度实现，使用已有 raw
trace 在用户态 runner 中重放。建立逐帧 contacts、slots、坐标和 baseline hash 的一致性
测试。此阶段不新增设备节点。

完成标准：同一 fixture 的内核 host test 与用户态 runner 输出一致，且有直线、圆、边缘、
split、silent gap、wake/noise fixtures。

### 阶段 B：只读 raw stream

实现版本化 capability、frame header、预分配队列和 `read/poll`，只供诊断 daemon 读取，
内核仍唯一上报触控。验证 120 Hz 下的带宽、丢帧、功耗、suspend 和进程退出行为。

完成标准：不开客户端时热路径成本接近零；客户端持续读取不改变内核坐标和 slot；故意让
客户端停读不会阻塞 IRQ。

### 阶段 C：影子求解

daemon 实时求解但不控制 input。内核 diagnostics 关联比较 kernel/offload 坐标、触点数、
tracking ID、耗时和异常原因。

完成标准：长时间运行没有队列延迟增长；定量证明用户态算法在目标场景优于内核路径；所有
差异能回放解释。

### 阶段 D：实验性结果回交

增加 `REPORT` ioctl 和显式 offload mode，只在诊断/profile 开关下启用。实现 slot 校验、
deadline、daemon death 和 suspend/resume fallback。

完成标准：kill/stop/卡死 daemon、乱序/重复/越界结果、反复亮灭屏都不会让触控永久失效；
切换最多产生一次有记录的保守 release，不能产生鬼触。

### 阶段 E：可选生产路径

只有在 Linux 和 Android 上完成延迟、功耗、可靠性和安全回归后，才考虑默认启用。必须继续
保留内核 fallback，并冻结/版本化 UAPI 和算法 profile。

## 10. 何时不应该做 offload

出现以下情况时，应继续改进内核算法而不是启动架构迁移：

- 当前目标只是三点插值、四边补偿或正确的定点 1-Euro；
- profiling 表明 threaded IRQ 时延充足，代码仍容易测试；
- Android vendor daemon、SELinux 和分区同步成本高于算法收益；
- 没有可重放 trace 和量化指标，无法证明用户态实现更好；
- 设备必须在极早启动、recovery 或无用户态服务时保持完全一致的高级触控行为。

offload 不是性能优化的同义词。它会增加调度、上下文切换、拷贝、ABI 和生命周期复杂度。
其价值在于隔离复杂算法、改善可测试性和迭代速度，并允许安全地引入不适合内核的能力。

## 11. 当前决策

在完成 [逆向证据与移植路线图](windows-porting-roadmap.md) 中近期的帧健康、面板 profile、
空间坐标和时间滤波工作前，不将 offload 设为当前默认开发主线。近期代码应保持未来可迁移：

- 硬件访问与纯算法分层；
- 算法不直接依赖 `input_dev`、SPI 或 sysfs；
- frame 输入和 contact 输出结构具有清晰所有权；
- 新特性配套 raw fixture 和 host regression；
- diagnostics 保存 frame index、timestamp、baseline/track 证据。

当计划移植完整 TSACore 级 baseline/异常分类，或内核算法已经显著限制调试和迭代时，再从
阶段 A 开始实施。不要直接从一个 root APK 读取 sysfs 并用 `sendevent` 模拟触控；那无法
提供可靠 deadline、单一 input device、生命周期和故障回退。
