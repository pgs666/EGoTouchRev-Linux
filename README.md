# EGoTouchRev Linux

面向华为 MateBook E Go 2023（Gaokun）Himax HX83121A TDDI 的实验性 Linux/Android
触控驱动。本仓库包含 SPI 内核模块、可在主机运行的算法回归测试、诊断 APK 和调参
工具。

驱动目前只上报手指 Type-B 多点触控，不支持原厂 Windows 栈中的主动笔协议。代码
仍处于实机验证阶段，不应视为通用 Himax 驱动或生产级硬件支持。

## 当前实现

- HX83121A SPI/AHB 访问、固件 reload、raw event-stack 读取；
- panel suspend/resume 与完整硬件恢复；
- 唤醒 raw-quality qualification 和 safe baseline 保护；
- 40×60 raw matrix 的 baseline、CMF、zone、palm、peak 和坐标求解；
- persistent peak identity、Hungarian assignment、silent-gap 与 Type-B slot 上报；
- 可编译关闭的完整事件栈/矩阵/轨迹诊断 ring；
- 不依赖触控硬件的算法 fixture。

实现边界与并发约束见 [驱动架构](docs/architecture.md)，后续选择性移植顺序见
[Windows 特性路线图](docs/windows-porting-roadmap.md)。

## 仓库结构

```text
touchscreen-hx83121a-dkms/  内核模块和 DKMS 配置
tests/                       纯算法 host regression
scripts/                     本机安全部署脚本
himax-touch-diagnostic/      手动故障采集 Android 应用
android-tuner/               Android root 调参和诊断应用
tuner/                       Linux sysfs 图形调参工具
docs/                        架构和后续开发文档
```

APK、内核模块、采集日志和本机部署记录是构建/运行产物，不纳入源码版本控制。

## 构建

要求当前 Linux 内核的 headers/build tree，以及常规内核模块构建工具。顶层目标会先
运行算法测试，再构建普通模块：

```sh
make
```

分开执行：

```sh
make test
make driver
make check       # clean host tests + clean module build + diff whitespace
```

模块位于 `touchscreen-hx83121a-dkms/himax-spi.ko`。直接在驱动目录构建诊断版：

```sh
make -C touchscreen-hx83121a-dkms HIMAX_DIAGNOSTICS=1
```

普通构建默认不包含 `event_trace`、完整 event stack sysfs 和逐 IRQ 大块复制。只有在
复现硬件/算法故障时才应部署诊断版。

### DKMS

```sh
sudo make -C touchscreen-hx83121a-dkms dkmsinstall
sudo make -C touchscreen-hx83121a-dkms dkmsremove
```

### 本机部署

```sh
make -C touchscreen-hx83121a-dkms deploy-local
```

部署脚本会构建并测试、保存当前模块与 sysfs 参数、安装 DKMS 模块、校验磁盘和运行
模块的一致性，并在失败时恢复旧模块。日志写入被忽略的 `deploy-logs/`。该命令会
重载当前系统的触控模块；它不会构建 Android 内核、写入磁盘槽位或重启设备。

## 运行时接口

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

## 来源与许可

代码采用 GPL-2.0。底层驱动最初参考 Himax/`linux-gaokun` 的 SPI 实现；手指算法与
恢复策略参考 EGoTouchRev Windows 驱动、厂商 Android 内核代码及对 Gaokun 原厂栈
的逆向分析。新增代码必须保留可追溯来源并遵守对应许可证，不能把反编译伪代码或
专有二进制直接复制进内核。
