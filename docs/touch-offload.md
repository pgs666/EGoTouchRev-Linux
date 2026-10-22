# HX83121A V4L2 用户态触控架构

2026-10-22：本分支采用“内核传输、用户态 TSACore、uinput 上报”。本文替代此前的
misc 字符设备/结果回交/运行时内核 fallback 草案。当前实现与旧内核算法以构建选项
互斥，不在运行中自动切换两个求解器。

## 数据边界

```text
SPI IRQ
  → threaded IRQ 完整读取 5132 字节
  → vb2 META_CAPTURE（HXST，MMAP）
  → 用户态校验 native master、前补 3 字节 THP 头
  → 原版 HimaxAfeAdapter + TSACore + TsaOutputMapper
  → uinput Type-B snapshot
  → Linux input/libinput
```

内核仅有 `himax-v4l2.c`、共享的 `himax-spi-hw.c` 和可单独 host 测试的 OTP 协议
`hx-otp.c` 三个编译单元。不链接 baseline、
peak detection、tracker、滤波、input 或旧诊断 ring。保留的硬件职责包括 GPIO reset、
AHB/SPI 事务、OTP 读取、reload、raw producer、panel follower、充电状态和命令邮箱。
这些必须由硬件拥有者串行执行，无法可靠地简化为毫无生命周期的 SPI 字节转发器。

用户态通过 submodule 编译原版全部启用算法，保留上游模块边界与面板参数表。
不把 C++ 浮点和进程级全局状态搬进内核。手写笔采集未启用。
这里的“原版”是上游 EGoTouchRev v1.4.0 的 C++ 重写实现，不能据此声称直接运行
华为原厂 DLL，或已经通过零售驱动的逐帧等价验证。

## V4L2 ABI v1

源定义以 [`hx-v4l2-uapi.h`](../touchscreen-hx83121a-dkms/hx-v4l2-uapi.h) 为准。
所有结构使用固定宽度字段、显式版本/长度和零保留字段，64 位字段显式对齐。

| 接口 | 内容 |
|---|---|
| `V4L2_BUF_TYPE_META_CAPTURE` / `HXST` | 一个 buffer 对应一个 5184 字节 `hx_raw_frame` |
| payload | 原样 5132 字节 SPI event stack，不在内核解析 suffix/checksum |
| epoch/sequence | 每次 AFE 重启/屏幕停止改变 epoch；成功读取后 sequence 递增 |
| timestamp_ns | IRQ 线程入口的 CLOCK_MONOTONIC 时间，不是硬件采样时间 |
| flags | AFE SOS 恢复原因、当前充电状态 |
| `VIDIOC_HIMAX_INFO` | ABI、网格 40×60、payload 长度、OTP project ID |
| `VIDIOC_HIMAX_COMMAND` | ClearStatus、频点、扫描率、IDLE、calibration、reset |
| `V4L2_EVENT_HIMAX_STATE` | running/epoch；即使屏灭不再产帧也能及时释放触点 |

命令必须来自当前 vb2 queue owner，并与当前 epoch、最近八帧内的 sequence 关联。
校验 action、参数和保留字段；普通 action 最短间隔 20 ms，reset 最短一秒。
这是邮箱传输确认，calibration-done 仍通过后续帧状态确认，不能把写入成功称为校准完成。

## 队列和时序

队列限制为 2～8 个 buffer，daemon 默认分配四个。没有可用 buffer 时仍排空 SPI，
丢弃该帧；硬件不等待算法线程。MMAP 避免 read 的 copy_to_user，但当前仍有
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

STREAMON、亮屏恢复和受限 reset 执行：

```text
关 IRQ / 停止事件
→ epoch++
→ reset/detect（最多三轮）
→ 首次 OTP 读取后再次 reset/detect
→ reload / MCU ready / raw runtime
→ 排空三帧
→ StartCalibration / 120 Hz
→ running 事件 / 开 IRQ
```

OTP 使用原版独立的 AHB burst、DD access 解锁和只读槽位选择流程，
并在成功和失败路径恢复 DD/safe-mode 状态；控制触发寄存器不强求等值读回。
本机已从槽位 `0x0f18` 读出 `W273AS2700`，算法据此选择参数。
寄存器协议测试还覆盖部分读取失败、清理失败和逐次总线操作故障注入。

收到新的 epoch 时，用户态释放旧 slot、通知 OnAfeStopped/OnAfeStarted；SOS 重启
额外通知 OnAfeSos。TSACore 对 calibration-done 上升沿、baseline reset、保留状态和
频点切换的处理直接沿用上游。不能把上游 profile 未启用的 SafeBaseline 五槽描述为
当前有效功能，也不能假设 reset 就能解决掌压亮屏。

充电变化通过 power_supply notifier 排队，在硬件互斥锁下更新 firmware，并把状态
随帧交给原版 adapter；没有在 IRQ 里查询整套电源属性。

锁顺序为 video_lock → op_lock → buffer_lock。IRQ 不获取 video_lock；
硬件事务不持 buffer_lock。停止/解绑先同步 IRQ，再归还队列。IRQ 和 notifier/work
在释放状态前注销，V4L2 引用计数保持已打开文件和 mmap 所需状态。

## 故障与恢复

- SPI 半帧不会进入 vb2；连续三次读取失败停止 IRQ 并重启 AFE。
- 用户态连续五次 native 校验失败或遇到 SOS，可申请受限 reset。
- 无有效新帧 250 ms 后释放已上报触点。
- 停屏事件立即释放，不等待这个超时。
- 进程正常退出或被杀时关闭 uinput，虚拟输入设备销毁。
- systemd 三秒 watchdog 覆盖算法挂住的情况，停止/休眠期间也持续喂狗。
- 运行中四秒没有新帧则退出，由 supervisor 重建 stream/AFE；允许原版 IDLE
  约三秒的 master 间隔，屏灭不计入超时；重复 running 事件不能掩盖停帧。
- 多个消费者不能同时拥有采集队列；不允许内核和用户态同时上报同一块触屏。

服务退出后桌面触控暂时消失，由 systemd 重建服务；没有运行时内核求解 fallback。
保留旧实现的价值是显式恢复构建/部署，不是假装两套 baseline 可以无缝交接。

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
