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
[`hx-raw-init.c`](../touchscreen-hx83121a-dkms/hx-raw-init.c)。默认 V4L2 路径先执行
`hx_raw_prepare_scan()`，对应 Windows `Chip::Init()` 实际调用的前置事务：

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

然后执行 `hx_raw_power_on()`，顺序为：
RawOut 清零 → sorting 清零并读回 → N-frame=1 → reload 状态清零 →
平台 sense-on/reset → A8 低字节为 5 → 完整 reload word 为 `0x72c0` →
RawOut=`0xf6` → master SRAM password=`0x5aa5`。任何阶段的传输失败立即退出，
上层只能从完整硬件 reset 重试；sorting 最多三次，两个 ready 阶段各最多三十次。
reload 超时统一返回 `ETIMEDOUT`，错误日志给出失败阶段。

RawOut 在 sense-on 之前清零来自固定版本 Windows
`HimaxChipLifecycle.cpp::himax_mcu_power_on_init()`；此前 Linux 漏了这一步。
power-on host 测试运行的就是内核共享实现，覆盖寄存器顺序、每笔传输失败、状态迟到、
完整 reload word 检查和超时后禁止启用 raw producer。它使用人工核对的协议 fixture，
并未像邮箱构包测试那样直接编译 Windows 函数；不能声称验证了整个启动流程逐笔等价。

前置准备另有直接源码对照：测试构建通过
[`RawPrepareReference.cmake`](cmake/RawPrepareReference.cmake)，提取未经修改的
`himax_mcu_reload_disable`、`himax_mcu_assign_sorting_mode`、`hx_set_N_frame`、
`himax_switch_mode_inspection` 和 `write_and_verify` 五个函数体；寄存器值也从上游
`HimaxRegisters.cpp` 提取。五份输入文件（含实际使用的 `HimaxByteUtils.h`）都校验
完整 SHA-256，Init 调用顺序也受检查。仅 Windows HAL 读写替换成与 Linux 共用的
fake bus；每笔完整传输、返回值、20/10 次重试边界、N-frame 两段失败和高字不同均
与生产 C 实现比较。引用实现仅链接到测试，不进入模块或 daemon。

完整启动仍有差异：Linux 在前面做 IC 探测/进入 safe mode；保留额外的 A8 ready gate；
后面 `himax_restore_raw_runtime()` 仍会重放 AP notify、sorting/N-frame、RawOut、USB profile
及 SRAM password。前置 inspection/N-frame 的事务对照不涵盖这些额外检查、GPIO reset
或完整生命周期，也不能据此声称手动切频已经生效。slave/pen 通道按本项目范围保持关闭。

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
- `FILE.hxst.events`：HXSE v1 侧车，保存每帧使用的方向、求解结果摘要、AFE 请求及
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
