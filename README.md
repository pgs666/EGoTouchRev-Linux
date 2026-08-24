# EGoTouchRev-Linux

Linux DKMS 内核驱动，适用于 **华为 Matebook E Go 2023** 的 **Himax HX83121A** 触控芯片，配套图形化调参工具。

算法部分对齐 [EGoTouchRev Windows v1.1.2](https://github.com/awarson2233/EGoTouchRev/releases/tag/v1.1.2) 的触摸流水线，在遵循 Linux 内核约束（无浮点、固定容量运行态、IRQ 上下文精简）的前提下移植：逐像素自适应基线、边缘信号增强、CMF 共模滤波、GridIIR 时域滤波、8-连通宏区域 BFS、掌压分类、非对称峰值检测、加权质心定位、断点重连与速度预测跟踪器。

> [!NOTE]
> 仅移植**触摸（手指）流水线**。Windows 侧的 HPP2/HPP3 手写笔 / 触笔（stylus）抑制逻辑**未实现**，输入设备只注册 `MT_TOOL_FINGER`。

提供更好的日常使用和游戏游玩体验。

> [!WARNING]
> 本驱动代码完全由 Claude Opus 4.6 大模型进行编写，不提供任何可用性保证。

---

## 目录结构

```
EGoTouchRev-Linux/
├── touchscreen-hx83121a-dkms/   # DKMS 内核模块
│   ├── himax-spi-core.c         # SPI 通信、固件初始化、电源管理、输入上报
│   ├── hx-algo.c                # 纯算法流水线（无 SPI / IRQ / input_dev 依赖）
│   ├── hx-algo.h                # 数据结构定义与公开 API
│   ├── hx-frame-status.h        # Linux 事件栈校验与固件手指状态解析
│   ├── Makefile
│   └── dkms.conf
├── scripts/
│   └── deploy-local.sh          # 带校验和自动回滚的本机部署脚本
├── tests/                       # 无硬件算法回归测试
└── tuner/
    └── tune.py                  # 图形化实时调参工具（Tkinter）
```

---

## 算法流水线

每次 IRQ 触发后，`himax_ts_thread()` 按以下阶段处理原始帧数据：

```
读取 SPI 原始帧
│
├─ Phase 0: 帧校验       校验 Linux 4 字节帧头、suffix 标记与 retry 状态
│                       无效帧保留当前轨迹，不进入基线或检测流水线
│
├─ Phase 1: 预处理
│   ├─ 1A. 基线减除      固件 hasFinger 驱动逐像素 Q8 自适应基线
│   ├─ 1B. CMF 共模滤波  消除充电器/行列耦合噪声（默认启用）
│   ├─ 1C. 边缘增强      边框像素信号放大，补偿边缘灵敏度下降（默认启用）
│   └─ 1D. GridIIR 时域  兼容调参开关（Windows v1.1.2 对齐配置中默认关闭）
│
├─ Phase 2: 触点求解
│   ├─ 2A. 宏区域 BFS    8-连通洪泛，分割高于阈值的连通域
│   ├─ 2B. 掌压抑制      多证据评分；强而尖锐的正常手指不再整区删除
│   ├─ 2C. 峰值检测      非对称局部极值 + 五级过滤（Z1/Z8/漂移/边缘/面积）
│   └─ 2D. 区域扩展+质心 BFS 扩展 → Q8.8 加权质心 → 设备原生坐标
│
├─ Phase 3: 触点跟踪
│   ├─ 3A. 距离匹配      速度预测 + 匈牙利全局最优 + 4 帧 silent-gap 重连
│   └─ 3B. input_mt 上报 10 槽 ABI（MT_TOOL_FINGER），可选压力/触摸面积
│
└─ 结束
```

**上报 ABI**：`input_mt_init_slots(…, HIMAX_MAX_TOUCH)` 固定注册 **10 个触控槽**
（`MT_TOOL_FINGER`），并启用 `input_mt_report_pointer_emulation` 模拟单指指针。

---

## 构建与安装

### 依赖

- Linux 内核头文件（与运行内核版本匹配）
- `dkms`（可选，用于持久化安装）
- Python 3.12+、`tkinter`（调参工具）

### 手动编译

```bash
cd touchscreen-hx83121a-dkms
make                         # 普通版，默认不包含诊断采集
# make HIMAX_DIAGNOSTICS=1   # 诊断版，包含完整 event_trace/event_stack
sudo insmod himax-spi.ko
```

完整诊断采集是编译期可选功能，普通版默认关闭。关闭时不会编译
`diagnostics`、`event_stack[_hex]` 和 `event_trace` sysfs 接口，不分配约
10.3 MiB 的 trace ring/snapshot，也不会在每个触控 IRQ 中复制原始事件栈和
2400 点处理矩阵。需要配合 Android 诊断 APK 抓取现场时，再显式构建诊断版。

### DKMS 安装（推荐）

```bash
cd touchscreen-hx83121a-dkms
sudo make dkmsinstall
```

卸载：

```bash
sudo make dkmsremove
```

### 本机安全部署（推荐）

```bash
make -C touchscreen-hx83121a-dkms deploy-local
# 需要诊断版时：HIMAX_DIAGNOSTICS=1 ./scripts/deploy-local.sh
```

该目标先跑 `test all` 再执行 `scripts/deploy-local.sh`，把“已知可用”的旧模块
备份下来，部署新模块并在任一步失败时**自动回滚**。流程：

1. **备份与快照**：把当前已加载的 `himax_spi.ko` 复制到临时目录，并逐项保存
   `/sys/bus/spi/devices/spi0.0/algo/` 下的全部算法参数（TSV）。
2. **DKMS 0.1 安装**：同步源码到 `/usr/src/himax-spi-0.1`，写入
   `/etc/modprobe.d/himax-spi-dkms.conf`，`dkms install --force himax-spi/0.1`。
3. **产物一致**：把已测试通过的仓库 `.ko` 覆盖到 DKMS 安装路径并 `depmod`；
   比对仓库与磁盘模块的 SHA-256，并用 Build ID 确认当前运行模块来自该产物。
4. **重载与恢复**：`modprobe -r` 再 `modprobe` 加载新模块，随后回写第 1 步保存的
   sysfs 算法参数（失效值会跳过）。
5. **验证**：确认 `spi0.0` 绑定驱动为 `himax-spi`、输入设备存在；比对磁盘模块
   SHA-256 与仓库构建一致；比对运行内核模块的 Build ID 与磁盘一致；打印内核日志。
6. **失败回滚**：任一步骤失败即 `modprobe -r`、恢复备份的 `.ko`、`depmod` 并重新
   加载旧模块，脚本以非零码退出。

全程日志写入 `deploy-logs/<时间戳>.log`，末尾 `deployment verified` 表示成功。

### 模块参数

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `disable_pressure` | bool | `true` | 禁用 `ABS_MT_TOUCH_MAJOR` 和 `ABS_MT_PRESSURE` 轴 |

---

## 运行时调参（sysfs）

驱动加载后，算法参数通过 sysfs 实时可调，无需重新加载模块：

```bash
# 查找 sysfs 节点
SYSFS=$(find /sys/devices -name 'cmf_enabled' 2>/dev/null | head -1 | xargs dirname)

# 读取当前值
cat $SYSFS/cmf_enabled

# 写入新值（需 root）
echo 0 | sudo tee $SYSFS/cmf_enabled      # 关闭 CMF
echo 400 | sudo tee $SYSFS/peak_threshold  # 降低峰值阈值
```

### 主要可调参数

#### 预处理

| 属性 | 默认值 | 说明 |
|------|--------|------|
| `baseline_enabled` | `1` | 逐像素自适应基线开关 |
| `baseline_noise_deadband` | `90` | 基线噪声死区 |
| `baseline_peak_threshold` | `305` | 真实触摸冻结基线的阈值 |
| `baseline_release_hold_frames` | `60` | 抬手负回弹期间的基线保持帧数 |
| `cmf_enabled` | `1` | CMF 共模滤波开关 |
| `cmf_exclusion` | `2000` | 超过此信号的像素不计入 CMF 均值 |
| `cmf_max_correction` | `2000` | 每行/列修正量上限 |
| `iir_enabled` | `0` | IIR 时域滤波开关（保留兼容，默认不进入活动流水线） |
| `iir_decay_weight` | `200` | 混合权重（0=全历史，256=不混合）|
| `iir_decay_step` | `80` | 每帧自然衰减量 |

#### 检测

| 属性 | 默认值 | 说明 |
|------|--------|------|
| `macro_threshold` | `280` | BFS 种子最低信号 |
| `peak_threshold` | `280` | 峰值候选最低信号 |
| `peak_signal_threshold_limit` | `1000` | 压力漂移检测的独立信号阈值基准 |
| `palm_enabled` | `1` | 掌压抑制开关 |
| `palm_area_threshold` | `50` | 像素数 ≥ 此值判为手掌 |
| `edge_comp_enabled` | `1` | 边缘补偿开关 |
| `edge_boost_pct` | `50` | 边缘像素信号增强百分比 |

#### 跟踪

| 属性 | 默认值 | 说明 |
|------|--------|------|
| `track_dist2_max` | `176400` | 最大匹配距离²（420²，约 6 格）|
| `track_lost_frames` | `4` | silent-gap 重连窗口，超出才释放槽 |
| `debounce_base` | `1` | 新触点正式上报前的稳定帧数 |
| `track_smoothing` | `1` | 速度自适应定点坐标平滑 |

### 与 Windows v1.1.2 同步的断触修复

* 候选峰仍按原算法从弱到强处理，但会先求解全部 20 个候选，再按
  `signal_sum` 保留最强的 10 个。旧代码先截取升序列表前 10 个，噪声较大时
  会误删真正的强手指峰。
* 掌触判断从“任一阈值命中就删除整个区域”改为多证据评分，并保留强尖峰，
  避免重按或大拇指被误判成手掌。
* 单次 SPI 读帧失败不再立即复位控制器；连续 3 帧失败才复位，期间保留轨迹。
* 解析 Linux 原生事件栈中的固件 `hasFinger`；错位、非 master 或 retry 帧不会
  进入基线和触点检测，也不会推进轨迹丢帧计数。
* 轨迹允许 4 帧 silent gap（`track_lost_frames`），并按缺失帧数外推预测位置、
  放宽重连门限，抬指短暂丢失不立即释放槽位。

### 算法回归测试

无需触摸硬件即可在主机运行固定输入测试：

```bash
make -C tests test
```

测试覆盖候选容量、超大 zone、掌中强手指与 PalmBox、多峰鞍点拆分、边缘补偿/
拒绝、自适应基线、Hungarian 交叉 ID、动态防抖、同行鬼点、1-Euro、silent gap、
运行态重置以及点击/拖动/长按/释放生命周期。

---

## 图形化调参工具

```bash
cd tuner
python tune.py
```

工具会自动检测驱动 sysfs 节点，提供分标签页的参数面板，支持以下模式快捷切换：

- **日用模式**：恢复默认轨迹参数（轨迹平滑开、起始防抖 1 帧、防抖基准 1 帧、Euro 过滤开）
- **游戏模式**：关闭轨迹平滑、起始/基准/弱信号/边缘防抖全部置 0、启用跳点检测
  （`track_jump_dist2=6400`）、关闭速度自适应坐标平滑；保留匈牙利匹配与鬼点过滤

> 工具需要 root 权限写入 sysfs，启动时会自动通过 `pkexec` 提权。

---

## 验证

### 手工 evtest 验收清单

先定位触摸设备（`/proc/bus/input/devices` 中 Name 含 Himax/Touch 的那项），例如：

```bash
DEV=$(grep -B1 -A6 -i 'Himax\|Touch' /proc/bus/input/devices \
      | awk '/Handlers=/{print $2}' | head -1)   # 得到 eventX
sudo evtest /dev/input/$DEV
```

逐项核对：

1. **单指**：手指按住 → `ABS_MT_TRACKING_ID` 与 `ABS_MT_POSITION_X/Y` 稳定上报，
   坐标随移动连续变化，抬手出现 `-1` 释放。
2. **多指**：双指同放 → 两个不同 `TRACKING_ID` 槽位并存；双指对向滑动无鬼点
   抖动、无槽位 ID 漂移。
3. **10 槽上限**：第十指仍能新建槽位，第十一触不再上报（`HIMAX_MAX_TOUCH=10`）。
4. **掌压**：整手掌压下不应产生跳动的多指；重按或大拇指不应被误删。
5. **边缘**：面板四边（尤其角落）触摸能定位、无死区。
6. **充电噪声**：接入充电器后未触摸时不应出现自发触点；可短暂关闭 CMF
   （`echo 0 > $SYSFS/cmf_enabled`）对照观察，完成后恢复为 `1`。
7. **断点重连**：快速抬指再落指（4 帧内）槽位 ID 应保持，而非重建新 ID。
8. **Suspend/Resume**：合盖再开后无需重插即可继续正常上报。
9. **压力/触摸面积**：默认关闭（`disable_pressure=true`），加载时 dmesg 应提示
   `pressure/touch_major axes disabled`；改参数 `disable_pressure=false` 后才上报
   `ABS_MT_TOUCH_MAJOR` / `ABS_MT_PRESSURE`。

编译快速自检（本机可能显示内核构建编译器或 `pahole` 版本差异警告）：

```bash
make -C touchscreen-hx83121a-dkms clean all
```

---

## Credits

| 角色 | 说明 |
|------|------|
| **算法实现** | [Claude Opus 4.6](https://www.anthropic.com/claude) — 本驱动触控算法（`hx-algo.c/h`）由 Claude Opus 4.6 大模型编写 |
| **算法参考** | [awarson2233/EGoTouchRev](https://github.com/awarson2233/EGoTouchRev) — Windows 用户态触控服务，提供 CMF、GridIIR、宏区域检测、掌压抑制、峰值检测、加权质心、触点跟踪等完整算法参考 |
| **原始驱动** | [TheUnknownThing/linux-gaokun](https://github.com/TheUnknownThing/linux-gaokun) — Linux SPI 多点触控驱动基础，提供 SPI 通信、固件初始化、电源管理、panel follower 等底层实现 |
