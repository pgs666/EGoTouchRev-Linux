# EGoTouchRev-Linux

面向华为 MateBook E Go 2023（Gaokun）Himax HX83121A TDDI 的实验性 Linux/Android
触控驱动。本仓库包含 SPI 内核模块、可在主机运行的算法回归测试、诊断 APK 和调参
工具。

默认使用 V4L2 原始帧采集 + 用户态 TSACore + uinput 手指 Type-B 多点触控，
不支持原厂 Windows 栈中的主动笔协议。代码
仍处于实机验证阶段，不应视为通用 Himax 驱动或生产级硬件支持。

## 当前实现

- HX83121A SPI/AHB 访问、固件 reload、raw event-stack 读取；
- panel suspend/resume 与完整硬件恢复；
- V4L2 metadata/HXST 有界采集队列、epoch/单调时间戳和受限 AFE 命令；
- 用户态直接编译上游 v1.4.0 的完整已启用 TSACore、adapter、面板参数和输出映射；
- 独立桌面方向桥接、启动方向缓存握手，以及两处经过源文件哈希检查的初始化/复位方向适配；
- uinput 唯一输入设备、systemd watchdog、HXST 录制与旧 trace 回放；
- 独立 legacy 构建保留原内核算法和诊断 ring，供开发恢复；
- 不依赖硬件的算法、输入生命周期及帧协议回归测试。

实现边界与并发约束见 [驱动架构](docs/architecture.md)，已审计的热路径与待验证候选
见 [性能审计](docs/performance.md)，后续选择性移植顺序见
[逆向证据与移植路线图](docs/windows-porting-roadmap.md)。当前 V4L2/用户态链路的边界、
实时性和恢复要求见
[触控算法 Offload 架构参考](docs/touch-offload.md)。
旧架构和性能审计文档描述 legacy 路径，新入口以
[用户态运行说明](userspace/README.md) 为准。

## 仓库结构

```text
touchscreen-hx83121a-dkms/  内核模块和 DKMS 配置
tests/                       纯算法 host regression
userspace/                   原版 TSACore、V4L2 daemon、uinput、录制/回放
third_party/EGoTouchRev/      Windows 上游 EGoTouchRev 的固定版本 submodule
scripts/                     本机安全部署脚本
himax-touch-diagnostic/      手动故障采集 Android 应用
android-tuner/               Android root 调参和诊断应用
tuner/                       Linux sysfs 图形调参工具
docs/                        架构和后续开发文档
```

APK、内核模块、采集日志和本机部署记录是构建/运行产物，不纳入源码版本控制。

## 构建

要求当前内核 headers/build tree、C++23 编译器、CMake、pkg-config、libsystemd 开发文件
和测试所需的 Python 3。默认同时构建 V4L2 内核模块、
用户态程序并执行回归测试：

```sh
git submodule update --init third_party/EGoTouchRev
make
```

分开执行：

```sh
make test
make driver
make check       # clean host tests + clean module build + diff whitespace
```

模块位于 `touchscreen-hx83121a-dkms/himax-spi.ko`，用户态程序位于 `userspace/build/`。
默认模块不含触控算法，必须运行 `himax-touchd` 才产生触摸输入。它需要
`CONFIG_INPUT_UINPUT` 和 V4L2/vb2-vmalloc 支持；依赖、安装和采集方法见
[用户态说明](userspace/README.md)。

旧内核算法需显式选择；其诊断版构建为：

```sh
make -C touchscreen-hx83121a-dkms HIMAX_V4L2=0 HIMAX_DIAGNOSTICS=1
```

旧普通构建不包含诊断 ring。新 V4L2 构建每帧转交原始数据是正常输入所需，
没有额外 legacy 诊断 ring。现有 APK/sysfs 调参工具尚未适配新接口。

legacy 开发版默认启用实验性的多槽 SafeBaseline、held-in-hand reacquire、SHB 和
BLRecal 路径。它们可以作为一个整体关闭，同时保留基础 working baseline、BLIIR/BLSM
和 BLReset 检测：

```sh
make -C touchscreen-hx83121a-dkms HIMAX_V4L2=0 HIMAX_EXPERIMENTAL_SAFE_BASELINE=0
```

该开关仅适用于 legacy。新路径直接使用上游 profile 实际启用的 baseline 行为，
不额外启用五槽扩展。掌压亮屏仍是已知未解决场景，Windows Gaokun 原版驱动也能
复现；默认启用不代表这些扩展路径已经达到原厂等价或正式稳定状态。

### DKMS

```sh
sudo make -C touchscreen-hx83121a-dkms dkmsinstall
sudo make -C touchscreen-hx83121a-dkms dkmsremove
```

这只安装内核模块，不安装/启动用户态服务。完整部署使用下一节脚本。

### 本机部署

```sh
bash scripts/deploy-v4l2-local.sh --trial  # 短时验证后恢复原模块
bash scripts/deploy-v4l2-local.sh          # 安装 V4L2 + 用户态服务
```

部署脚本会构建并测试、保存当前模块、验证实际采集和 uinput，并在失败时恢复旧模块。
日志和备份写入被忽略的 `deploy-logs/`。该命令会
重载当前系统的触控模块；它不会构建 Android 内核、写入磁盘槽位或重启设备。

## 运行时接口

以下参数和 sysfs 仅适用于 `HIMAX_V4L2=0`。新路径使用 V4L2 与用户态服务接口。

算法参数位于 SPI 设备的 `algo/` sysfs group。实际设备路径由内核设备拓扑决定，
不要在脚本中写死：

```sh
node=$(find /sys/bus/spi/devices -path '*/algo/peak_threshold' -print -quit)
algo_dir=${node%/*}
cat "$algo_dir/peak_threshold"
printf '%s\n' 300 | sudo tee "$algo_dir/peak_threshold"
```

所有参数定义、范围和默认值以 `hx_algo_init_defaults()` 及 sysfs attribute 表为准。
README 不复制一份容易过期的参数表。修改参数只能用于受控 A/B；确认有效后应同步到
代码默认值并补充测试。

模块参数：

- `disable_pressure=true`：不注册 pressure/touch-major 轴；
- 主动笔支持默认关闭（`HIMAX_STYLUS=0`）；当前 mutual-cap 数据不会被当作笔
  事件解析。`HIMAX_STYLUS=1` 仅编译独立的预留钩子，尚不提供笔设备；
- `restore_afe_runtime=true`：硬件初始化后重放 120 Hz AFE 命令；
- `full_resume_on_panel_enable=true`：每次 panel enable 走完整恢复和 wake qualification。

## 诊断与调参工具

- `himax-touch-diagnostic/`：断触后手动冻结驱动 ring，并采集随后数秒的 input、
  event-stack、dmesg 和 Android 输入栈信息；具体操作见其 README。
- `android-tuner/`：需要 root 的 Android 参数编辑器，包含 daily/game profile 和诊断
  入口；构建方法见其 README。
- `tuner/`：Linux Tk 图形前端，直接读写算法 sysfs。建议使用项目声明的 Python
  环境运行，不要以 root 启动整个图形会话。

Android 工程不提交 Gradle wrapper、SDK、缓存或预编译 APK。默认 APK 输出均位于各
工程的 `app/build/outputs/apk/debug/`。

## 验证要求

代码提交的最低门槛是：

```sh
make check
```

算法变更必须增加 fixture，并至少覆盖相关的存在性判断、slot 生命周期和异常帧行为。
硬件恢复变更还需要诊断版实机验证以下场景：冷启动、连续亮灭屏、亮屏时手已按下、
充电/未充电、快速边缘手势和多指交叉。一次正常唤醒不能证明恢复路径已经稳定。

开发规范和提交边界见 [CONTRIBUTING.md](CONTRIBUTING.md)。
