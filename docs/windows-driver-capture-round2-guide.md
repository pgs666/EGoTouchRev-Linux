# HX83121A Windows 原版驱动第二阶段采集要求

日期：2026-09-06

## 1. 本轮目标

上一轮四份 ETL 完整且没有事件丢失，但没有取得 raw/mutual-cap、baseline 内容或
逐帧 BLSM/BLReset 状态；其中“亮屏前按住手指/手掌”两轮还因为息屏期间
`Console.Beep` 延迟而无法证明操作顺序。

本轮目标不是再次收集大量普通 ETL，而是依次完成：

1. 用 WPA 从已有 ETL 恢复真实亮灭屏和原厂触控栈调用时间线；
2. 使用可靠的外部提示重采手指/手掌亮屏实验；
3. 依据静态 RVA，在 WinDbg 中对原厂用户态进程进行一次受控、只读的状态采集；
4. 尽量取得可与同一帧关联的 raw、baseline、SafeBaseline、BLSM 和 BLReset 数据。

禁止修改未知内存、寄存器、SPI 数据和驱动配置。不要启用 Driver Verifier/WDF
Enhanced Verifier，也不要进行内核调试。

## 2. 输入文件与已知事实

使用桌面归档：

```text
HX83121A-Windows采集与后续建议-20260906.zip
```

总归档 SHA-256：

```text
5e40243fbad02b76b08491d329b3b5587a1672c4d807231cf94db49714de71f9
```

已确认原厂 DLL 哈希：

```text
TSACore.dll       e9597b2ce3370a6dc976c3163d2a65479c61206a1eca73f55e7de6e20567f840
himax_thp_drv.dll f3c8376df0b656e9fefceb7ad99b10ad0ec85c0a76237869c787d89fa5a8b063
```

`GaokunThpHost.exe` 是 ARM64EC 最终映像，加载原厂 x64 DLL。必须使用支持
ARM64EC/ARM64X 的新版 WinDbg；不要假设传统 x64 ABI 在所有边界都成立。

## 3. 阶段 A：用 WPA 分析已有 ETL

安装 Windows Performance Toolkit，只需 WPA。不要开启额外验证器。

分别打开四份 ETL，建立以下视图：

- System Activity / Process Lifetimes；
- Computation / CPU Usage (Sampled)，启用 Stack；
- Power / Device D-State、Modern Standby、Display/Monitor 状态；
- WDF I/O、ISR、DPC 和 Passive ISR；
- Image Load。

过滤进程/模块：

```text
GaokunThpHost.exe
OpenEGoHubService
THP_Service.dll
TSACore.dll
himax_thp_drv.dll
SpiModule.dll
```

需要导出的结果：

```text
wpa/round1-display-power.csv
wpa/round1-touch-stack.csv
wpa/round2-display-power.csv
wpa/round3-display-power.csv
wpa/round4-touch-stack.csv
wpa/notes.md
```

`notes.md` 必须记录：

- ETL 中真实的 display-off、display-on 时间；
- `GaokunThpHost` 在亮屏前后首次恢复活动的时间；
- WDF ISR/DPC 停止和重新出现的时间；
- 栈中是否出现目标 DLL，以及模块内偏移；
- WPA 表名、过滤条件、时间区间和排序列。

若 WPA 只能看到模块偏移而不能解析函数名，保留完整 `module+0xoffset`，不要猜函数。

## 4. 阶段 B：重采正确的亮屏实验

必须重采以下三轮：

1. 无触摸灭屏后亮屏；
2. 灭屏时先放置食指并保持，再按电源键亮屏，保持 3 秒后松开；
3. 灭屏时先放置手掌并保持，再按电源键亮屏，保持 3 秒后松开；松开后继续观察
   20 秒并滑动测试。

不要使用息屏期间的本机蜂鸣作为提示。使用第二台设备的倒计时、录像或第二位观察者。
推荐同时录像，画面中包含：

- 第二台设备的秒表；
- 手指/手掌何时接触和离开；
- 本机屏幕亮起时刻；
- 触控开始异常的时刻。

每次手动按键时，立即用另一输入设备执行一个不会阻塞的时间标记，例如：

```powershell
$sw = [Diagnostics.Stopwatch]::StartNew()
function Mark($name) {
  "{0},{1:o},{2}" -f $sw.ElapsedMilliseconds,(Get-Date),$name |
    Add-Content .\timeline.csv
}
```

可以由外接键盘在 PowerShell 中执行 `Mark 'palm_down'`、`Mark 'power_pressed'`、
`Mark 'display_visible'`、`Mark 'palm_up'`、`Mark 'fault_visible'`。人工标记最终必须用
WPA 的显示电源事件复核。

每轮单独保存 ETL、timeline 和录像说明，不要覆盖上一轮。

## 5. 阶段 C：受控 WinDbg 用户态只读采集

### 5.1 前置条件

- 已有外接鼠标/键盘，触控暂停时仍能恢复；
- 已记录服务重启命令；
- 已确认 `GaokunThpHost.exe` PID；
- 使用管理员权限运行新版 WinDbg；
- 一次只设置一个或少量断点，避免改变触控时序；
- 不执行任何写内存命令。

获取 PID：

```powershell
Get-CimInstance Win32_Process -Filter "Name='GaokunThpHost.exe'" |
  Select-Object ProcessId,ExecutablePath,CommandLine
```

附加后先执行并保存输出：

```text
.time
vertarget
|
lm m TSACore
lm m himax_thp_drv
x TSACore!BLIIR_*
x TSACore!BLSM_*
x TSACore!BLReset_*
x TSACore!SafeBaseline_*
x TSACore!g_*Baseline*
```

若 `x module!export` 可解析，应使用导出名；否则根据本包中的 RVA 加载地址计算实际
地址。每次必须记录 `lm` 给出的模块基址，禁止只记录绝对地址。

### 5.2 第一组低频断点：恢复流程

按顺序单独验证以下导出是否真的在亮屏路径命中：

```text
himax_thp_drv!thp_afe_start
himax_thp_drv!hx_hw_reset_ahb_intf
himax_thp_drv!hx_is_reload_done_ahb
himax_thp_drv!thp_afe_start_calibration
TSACore!BLIIR_Reset
```

每次命中只执行：

```text
.time
k
r
```

然后立即 `g`。不要在未确定参数 ABI 前读取大范围指针。需要回答：每次亮屏是否调用
硬件 reset、reload wait、calibration 和 BLIIR reset；调用顺序与间隔是多少。

特别注意：`thp_afe_start_calibration` 未命中同样是重要结果，不要强行触发它。

### 5.3 第二组断点：baseline 状态机

只有第一组稳定后再测试：

```text
TSACore!BLSM_Process
TSACore!BLReset_Process
TSACore!SafeBaseline_Process
TSACore!SafeBaseline_ScreenOnWithPhoneHeldInHandProcess
```

这些可能每帧调用。不要使用每次交互暂停的普通断点持续运行；优先使用带自动记录和
继续执行的命令断点，且仅保持 1～3 秒。例如：

```text
bp TSACore!SafeBaseline_ScreenOnWithPhoneHeldInHandProcess ".time; k 8; r; g"
```

若命中频率太高，立即 `bc *`，不要让调试器本身制造断触。

### 5.4 全局 baseline 导出

当前静态导出中存在：

```text
g_internalSafeBaseline  RVA 0x236180
g_safeBaseline          RVA 0x24C040
g_safeBaselineReset     RVA 0x24C160
g_safeBaselineResetLeft RVA 0x258500
g_safeBaselineResetRight RVA 0x258540
```

先用 `x` 和 `dq` 只读确认导出究竟是对象、结构体还是指针：

```text
x TSACore!g_safeBaseline
dq TSACore!g_safeBaseline L8
x TSACore!g_internalSafeBaseline
dq TSACore!g_internalSafeBaseline L8
```

不要直接把首个 qword 当成网格指针。只有满足以下条件才继续：

- 指针位于已提交可读内存；
- 多次读取结构稳定；
- 大小能从调用代码或分配函数证明；
- 数据形状与 HX83121A mutual-cap 网格一致；
- 同一地址在触控帧之间呈合理变化。

使用 `!address <addr>` 验证内存范围，并从较小长度开始 `db/dd/dw`。禁止盲目 dump
数 MB 内存。

### 5.5 必须重点验证的调用

还需确认以下函数是否真正启用于 Gaokun 配置：

```text
BLReset_IsTouchToBeProtected
BLReset_IsTouchToBeProtected2
BLReset_IsAllTouchByAbnormalBaseline
BLReset_IsAllFreqsBaselineInitializedDuringTouchLiftOff
SafeBaseline_CheckGhostMax
SafeBaseline_CheckGhostMaxInSilent
Touch_KeepPrevTouchWithExceptionExcluded
AFT_ReleaseRecheck
SignalDisparity_Process
```

仅“DLL 中有导出”不能证明运行时启用。对每个函数记录：

```text
tested / hit / not-hit / not-tested
实验轮次
命中次数或观察窗口
调用栈
模块基址与实际地址
```

## 6. 真正需要回传的数据

最高优先级：

- 同一操作序列中逐帧 raw/mutual-cap 网格；
- working baseline、当前 safe baseline、candidate baseline；
- SafeBaseline 五槽的地址/有效位/epoch/选择与淘汰情况；
- BLSM state、BLReset reason/timer；
- previous-touch/touch-property 统计；
- AFE reset/reload/calibration 的调用和完成状态。

若无法获取网格，也必须回传：

- 所有命中/未命中证据；
- WinDbg 命令日志；
- 模块基址、DLL 哈希和 RVA；
- 每个安全读取的地址、长度和数据类型依据；
- WPA 导出表及真实显示时间线。

## 7. 输出目录

```text
HX83121A-round2-YYYYMMDD-HHMMSS/
  manifest.txt
  SHA256SUMS.csv
  wpa/
  recapture/
    no-touch/
    finger-held/
    palm-held/
  windbg/
    session.txt
    hits.csv
    memory-layout.md
    dumps/
  conclusions.md
```

`conclusions.md` 必须严格区分：

- `exact`：日志、内存、反汇编或调用栈直接证明；
- `inferred`：多个证据支持但字段语义仍是推测；
- `not acquired`：尚未取得，不能用默认值填充。

## 8. 停止条件

出现以下情况立即清除断点、分离调试器并恢复服务：

- 触摸主机崩溃、VHF 设备消失或触摸不能恢复；
- 断点频率明显改变触控行为；
- 需要修改内存或 SPI 数据才能继续；
- 无法确定地址是否可读、对象大小或 ABI；
- 操作要求启用 Driver Verifier、内核调试或未知驱动选项。

本轮宁可明确写 `not acquired`，也不要将未启用的导出函数、猜测的结构体偏移或
错误时间线当作原厂行为。
