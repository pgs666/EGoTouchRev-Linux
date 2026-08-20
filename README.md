# EGoTouchRev-Linux

Linux DKMS 内核驱动，适用于 **华为 Matebook E Go 2023** 的 **Himax HX83121A** 触控芯片，配套图形化调参工具。

算法部分跟进 `EGoTouchRev` Windows 用户态驱动，在遵循 Linux 内核约束（无浮点、无动态内存、IRQ 上下文精简）的前提下移植：逐像素自适应基线、CMF 共模滤波、GridIIR 时域滤波、8-连通宏区域 BFS、掌压分类、非对称峰值检测、加权质心定位、断点重连与速度预测跟踪器。

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
│   ├── Makefile
│   └── dkms.conf
└── tuner/
    └── tune.py                  # 图形化实时调参工具（Tkinter）
```

---

## 算法流水线

每次 IRQ 触发后，`himax_ts_thread()` 按以下四个阶段处理原始帧数据：

```
读取 SPI 原始帧
│
├─ Phase 1: 预处理
│   ├─ 1A. 基线减除      逐像素 Q8 自适应基线（初值 0x7FEE），触摸区冻结
│   ├─ 1B. CMF 共模滤波  消除充电器/行列耦合噪声（默认启用）
│   ├─ 1C. GridIIR 时域  降低静态底噪，保护真实触控信号（默认启用）
│
├─ Phase 2: 触点求解
│   ├─ 2A. 宏区域 BFS    8-连通洪泛，分割高于阈值的连通域
│   ├─ 2B. 掌压抑制      多证据评分；强而尖锐的正常手指不再整区删除
│   ├─ 2C. 峰值检测      非对称局部极值 + 五级过滤（Z1/Z8/漂移/边缘/面积）
│   └─ 2D. 区域扩展+质心 BFS 扩展 → Q8.8 加权质心 → 输出坐标 [0, 65535]
│
├─ Phase 3: 触点跟踪
│   ├─ 3A. 距离匹配      速度预测 + 4 帧 silent-gap 重连，稳定多指 ID
│   └─ 3B. input_mt 上报 TOUCH_MAJOR / PRESSURE / pointer emulation
│
└─ 结束
```

---

## 构建与安装

### 依赖

- Linux 内核头文件（与运行内核版本匹配）
- `dkms`（可选，用于持久化安装）
- Python 3.12+、`tkinter`（调参工具）

### 手动编译

```bash
cd touchscreen-hx83121a-dkms
make
sudo insmod himax-spi.ko
```

### DKMS 安装（推荐）

```bash
cd touchscreen-hx83121a-dkms
sudo make dkmsinstall
```

卸载：

```bash
sudo make dkmsremove
```

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
| `iir_enabled` | `1` | IIR 时域滤波开关 |
| `iir_decay_weight` | `200` | 混合权重（0=全历史，256=不混合）|
| `iir_decay_step` | `80` | 每帧自然衰减量 |

#### 检测

| 属性 | 默认值 | 说明 |
|------|--------|------|
| `macro_threshold` | `280` | BFS 种子最低信号 |
| `peak_threshold` | `280` | 峰值候选最低信号 |
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

### 与最新版 Windows 实现同步的断触修复

* 候选峰仍按原算法从弱到强处理，但会先求解全部 20 个候选，再按
  `signal_sum` 保留最强的 10 个。旧代码先截取升序列表前 10 个，噪声较大时
  会误删真正的强手指峰。
* 掌触判断从“任一阈值命中就删除整个区域”改为多证据评分，并保留强尖峰，
  避免重按或大拇指被误判成手掌。
* 单次 SPI 读帧失败不再立即复位控制器；连续 3 帧失败才复位，期间保留轨迹。
* 轨迹允许 4 帧 silent gap，并按缺失帧数外推位置、放宽重连门限。

### 算法回归测试

无需触摸硬件即可在主机运行固定输入测试：

```bash
make -C tests test
```

测试覆盖候选容量溢出时保留强触点、强手指不被掌触规则删除、大面积平坦
接触被拒绝、自适应基线跟随面板漂移，以及四帧断点重连。

---

## 图形化调参工具

```bash
cd tuner
python tune.py
```

工具会自动检测驱动 sysfs 节点，提供分标签页的参数面板，支持以下模式快捷切换：

- **日用模式**：启用轨迹平滑、恢复默认防抖参数
- **游戏模式**：关闭轨迹平滑、禁用起始防抖、启用跳点检测

> 工具需要 root 权限写入 sysfs，启动时会自动通过 `pkexec` 提权。

---

## 验证

```bash
# 编译无警告
make -C /lib/modules/$(uname -r)/build M=$(pwd) 2>&1 | grep -i warning

# 加载无错误
sudo insmod himax-spi.ko && dmesg | tail -20

# 基本触控 / 多指 / 掌压
evtest /dev/input/$(ls /dev/input/ | tail -1)

# 充电噪声抑制：接入充电器，观察 evtest 输出是否稳定

# Suspend/Resume：合盖再开后 evtest 恢复正常

# sysfs 开关验证
echo 0 | sudo tee $SYSFS/cmf_enabled && evtest ...   # 关闭后噪声应增大
echo 1 | sudo tee $SYSFS/cmf_enabled                  # 恢复
```

---

## Credits

| 角色 | 说明 |
|------|------|
| **算法实现** | [Claude Opus 4.6](https://www.anthropic.com/claude) — 本驱动触控算法（`hx-algo.c/h`）由 Claude Opus 4.6 大模型编写 |
| **算法参考** | [awarson2233/EGoTouchRev](https://github.com/awarson2233/EGoTouchRev) — Windows 用户态触控服务，提供 CMF、GridIIR、宏区域检测、掌压抑制、峰值检测、加权质心、触点跟踪等完整算法参考 |
| **原始驱动** | [TheUnknownThing/linux-gaokun](https://github.com/TheUnknownThing/linux-gaokun) — Linux SPI 多点触控驱动基础，提供 SPI 通信、固件初始化、电源管理、panel follower 等底层实现 |
