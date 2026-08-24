# Himax Touch Diagnostic

这是一个不常驻、手动触发的 root 诊断 APK。断触发生后不要先重载驱动，保持现场并点“抓取证据”。每次采集会在 `/sdcard/HimaxTouchDiag/YYYYMMDD-HHmmss/` 写入：

- 点击瞬间之前约 4 秒的全速驱动追踪 `event-trace-before.bin`、约 2 秒时的 `event-trace-mid.bin`，以及观察窗口结束后立即保存的 `event-trace-after.bin`；这些重叠的 ring 快照覆盖完整观察窗口，避免 4 秒 ring 在较慢采样设备上覆盖故障现场。每条 IRQ 记录包含完整原始事件栈、基线处理后的 40×60 矩阵、峰、轨迹 slot、坐标、miss/debounce 状态和重置代次；
- 点击后 5 秒的 Linux input 事件 `getevent-post-window.txt`，用于确认 `ABS_MT_SLOT`、`ABS_MT_TRACKING_ID` 和同步事件是否真正中断或换轨；
- 完整的最后一次成功事件栈 `event-stack.bin`（驱动提供二进制 `event_stack` 节点时）；
- 点击后的真实 5 秒观察窗口：尽量每隔 20 ms 保存一次 `diagnostics` 和完整二进制事件栈到 `post-window/`；实际样本数取决于设备 sysfs 读取速度，并记录在 `post-window/status.txt`。点击后应继续保持滑动；
- 最近事件栈的十六进制文本摘要 `event-stack-hex.txt`；该摘要便于人工快速查看，但受 sysfs `PAGE_SIZE` 限制，可能不包含完整事件栈；
- `diagnostics` 和全部 `algo` sysfs 参数；
- `dmesg`、全 buffer `logcat`、`/proc/interrupts`、`dumpsys input`；
- 内核版本、启动参数、设备属性和 SPI sysfs 信息。

采集时会先读取 `event_trace` 冻结故障前现场，并立即启动 `getevent` 和 5 秒观察窗口；体积较大、速度较慢的 logcat/dumpsys/sysfs 枚举在窗口结束后执行。因此点击后应立刻继续滑动，不必等待界面显示完成。

界面支持平板横屏、竖屏和系统多窗口。旋转设备不会中止正在运行的 root 采集。

SPI/algo sysfs 属于辅助信息；如果设备的 Android `find`/sysfs 不提供这些节点，状态会记录为 `optional_*`，不会把核心触控证据判为失败。

“手动硬件重置并记录前后日志”只应在第一次采集完成后使用。它写入驱动已有的 `inplace_reset` 节点，并把 reset 前后日志放在同一个目录；不会执行模块卸载或加载。新版驱动还会在追踪记录中增加 `reset_generation`，并额外保存 `event-trace-after-reset.bin`。

## 解析全速追踪

在电脑上把二进制追踪转换成 CSV：

```sh
python3 decode_trace.py event-trace-before.bin event-trace-after.bin \
  -o event-trace.csv
```

CSV 会列出每帧的 SPI/master 状态、芯片有手指标志、算法峰数量、触点数量、所有峰的位置/强度，以及每个活动 slot 的位置、速度、年龄、miss 和 debounce。二进制文件本身还保留完整原始矩阵和算法处理矩阵，后续可以做空间热图或离线重放。

## 构建

本目录是独立 Android Gradle 工程，使用 Android Gradle Plugin 8.3.2、compileSdk 34。
需要 Android SDK 34、Build Tools 34.0.0、JDK 17 和 Gradle 8.6；可在带这些依赖
的环境执行：

```sh
./gradlew :app:assembleDebug
```

本项目不提交 Gradle wrapper、Gradle 缓存或预编译 APK。默认输出路径是
`app/build/outputs/apk/debug/app-debug.apk`。安装命令示例：

```sh
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

完整事件栈由驱动的只读二进制 sysfs 节点 `event_stack` 提供，全速环形追踪由 `event_trace` 提供。必须重新编译并部署包含这两个节点的 `himax-spi` 模块，APK 才能生成相应证据；核心节点不存在或读取失败会把采集状态标记为 `FAILED`。`event_trace` 在内核中保留最近 512 条 IRQ，通常约为 4 秒，并在读取时生成一致快照。原有 `event_stack_hex` 文本节点仍会采集为可读摘要。SPI/algo 辅助节点失败只记录为 `optional_*`，不会导致核心采集失败。
