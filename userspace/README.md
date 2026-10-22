# Linux 用户态触控求解

默认数据路径为：

```text
HX83121A SPI → V4L2 META_CAPTURE / HXST → himax-touchd → uinput → libinput
                  内核传输层              原版 TSACore
```

内核只负责 SPI/AHB、复位、固件启动、屏幕电源生命周期、充电状态和有界采集队列。
baseline、AFE 状态解析、噪声处理、检测、跟踪和滤波由用户态完成。内核不再注册
触摸 input device；`himax-touchd` 是唯一手指输入生产者。

算法直接编译上游 EGoTouchRev v1.4.0 的全部实际启用模块、`HimaxAfeAdapter`、
`TsaCoreSolver` 和参数表，submodule 固定在
`4d4f53d0bd8563d20850d5f380bcd6a8704c1e81`。不构建 Windows 服务、VHF 或 GUI。
主动笔硬件采集未启用，算法的笔状态保持无笔默认值。
文中“原版”指这个固定版本的上游 C++ 重写实现，不是直接加载华为 Windows DLL，
也不是 Himax 发布的算法源码；移植成功不等于已证明与零售版驱动逐帧等价。

## 构建

需要支持 C++23 的 GCC/Clang、CMake、当前内核 build tree。

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

脚本要求有现存、可恢复的未压缩模块；备份和日志保留在
`deploy-logs/v4l2-时间戳/`。`--trial` 不替换启动时使用的模块或启用常驻服务，
但会按需加载 uinput/videobuf2 依赖。
常驻安装会强制重建 DKMS 缓存，并核对仓库产物、缓存、磁盘模块和运行中 build ID；
失败时恢复原模块、DKMS 源码/注册状态和原服务状态。
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

服务用三秒 watchdog 监控整个求解循环，进程退出或被终止时关闭 uinput，销毁虚拟设备。
屏灭事件立即释放所有 slot；epoch、sequence 和时间检查禁止屏灭前队列帧重新生成触点。
250 ms 没有有效新帧时释放已上报触点。该超时是故障保护，不加入正常逐帧防抖。
运行状态下四秒没有新帧时服务以失败退出，由 systemd 重启并重新初始化 AFE；
该期限容纳原版 master IDLE 约三秒的间隔。屏灭期间不触发产帧超时。

## 原始帧采集和回放

观察模式独占 V4L2 队列，不创建输入设备，也不执行算法请求的 AFE 命令。
因此采集期间触屏不可操作，请使用键盘/鼠标；它不等价于正常服务下的 AFE 闭环。

```sh
sudo systemctl stop himax-touchd
sudo timeout --signal=TERM 5 userspace/build/himax-touchd \
    --observe --record /tmp/capture.hxst
sudo systemctl start himax-touchd
userspace/build/himax-replay --hxst /tmp/capture.hxst > /tmp/contacts.csv
```

正常服务不写逐帧文件；`--record` 仅允许观察模式，避免同步磁盘 I/O 进入输入路径。
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
CSV 坐标单位为面板原始方向的显示像素；uinput 使用其十倍量程，保留亚像素信息。
native master 只前补三个运输字节，不改变原始矩阵、suffix 和 checksum。
5132 字节 SPI 记录中，仅前 5060 字节属于 master，checksum 覆盖 `[2, 5060)`。
suffix 中的 `0xbb` 是校准完成状态，通过 ClearStatus(8) 确认，不能作为每帧固定 magic。

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

离线测试覆盖 native checksum、完整 TSACore 初始化及状态转换、Type-B ID 生命周期、
epoch/旧帧丢弃、产帧超时、HXST/HXTR 格式和截断文件处理，以及 OTP 完整协议和
逐次总线操作故障注入。已有真实 512 帧历史 trace 可完整回放。
这不证明掌压亮屏、快速滑动、多指和长期运行已经通过；不能据此宣称解决原有故障。

实时内核 transport 不做 raw checksum/峰值/基线判定；SPI 读取失败不会投递半帧，
连续三次失败触发受限重启。用户态校验失败或 SOS 请求通过版本化 V4L2 ioctl 申请恢复，
不提供任意寄存器写接口。完整接口、拷贝和故障模型见
[V4L2 架构说明](../docs/touch-offload.md)。

2026-10-22 本机 `7.1.8-gaokun3+` 验证结果：

- Release 与 ASan/UBSan 五组测试通过，旧算法 host regression 和 OTP host tests 通过；
- 旧 512 帧 trace、新采 509 帧 HXST 回放通过；新采帧 native 校验失败数为零；
- 实际 MMAP 采集、正常服务创建/销毁唯一输入设备、SIGSTOP 后 watchdog 终止、
  保持 fd/MMAP 时解绑与重新绑定通过；最终一轮无新内核 warning/oops；
- 20 秒服务测试最终处理 878 帧，solver 平均 144 µs、最大 1327 µs；
  此样本没有受控快速滑动/多指动作，不是触控手感、接触延迟或满负载基准；
- OTP 实际读出 `W273AS2700`、slot `0x0f18`、revision `0x46`、E2 `0x00`；
- 正常求解闭环观察到校准完成状态经 ClearStatus(8) 后消失，以及进入 IDLE；
  清状态、IDLE 命令各一次且传输成功，没有误触发四秒产帧超时；
- 常驻安装已通过，`himax-touchd.service` 已启用；唯一 uinput 设备被 libinput 识别为
  `267×167 mm` 的 touchscreen，运行中 build ID、磁盘模块和 DKMS 缓存一致；
- 已安装的受限 systemd unit 验证通过，SIGSTOP 后三秒 watchdog 杀死进程，
  服务自动重新初始化 AFE，并恢复唯一输入设备；
- 安装前曾因校准后首帧同步头无效触发严格检查失败，两次均自动恢复旧模块。
  现安装检查采用上述有界启动容忍，新样本 518 帧中首帧被明确拒绝，后 517 帧正常；
- 安装记录及旧模块/DKMS/配置备份：
  `deploy-logs/v4l2-20261022-225339/`。Android 未部署。

实际 suspend/resume、掌压亮屏、充电切换、方向/显示映射、快速滑动、多指和长期稳定性
仍未验收。早期首次加载曾因错误设置 vb2 最大队列长度触发 warning 并拒绝 probe，
随后已修正；该失败和自动回退不计为成功的实机验证。
