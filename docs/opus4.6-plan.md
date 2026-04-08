# HX83121A 触控算法重构规划

## Context

当前 `himax-spi.c` 中的触控识别算法非常基础：固定基线阈值、简单局部极值检测、4 点插值定位、最近邻跟踪。存在充电器噪声干扰、无掌压抑制、边缘精度差、ID 跳变等问题。

参考 `EGoTouchRev/EGoTouchService/Engine` 中成熟的 Windows 用户态驱动实现，在遵循 Linux 内核编程约束（无浮点、无动态内存、IRQ 上下文精简）的前提下，对算法部分进行重构。

**重构范围**：仅替换 `himax-spi.c` 第 880~1311 行的算法部分（从 `#define EQUILIBRIUM` 到 `himax_ts_thread` 结束）。SPI 通信、固件初始化、电源管理、panel follower 等全部保留不动。

---

## 关键文件

| 文件 | 角色 |
|---|---|
| `linux-gaokun/touchscreen-hx83121a-dkms/himax-spi.c` | 待重构的内核驱动（唯一改动文件） |
| `EGoTouchRev/.../Preprocessing/CMFProcessor.cpp` | CMF 算法参考 |
| `EGoTouchRev/.../Preprocessing/GridIIRProcessor.cpp` | 时域 IIR 参考 |
| `EGoTouchRev/.../Preprocessing/GaussianFilter.cpp` | 高斯滤波参考 |
| `EGoTouchRev/.../TouchSolver/MacroZoneDetector.cpp` | 连通域检测参考 |
| `EGoTouchRev/.../TouchSolver/PalmRejector.cpp` | 掌压抑制参考 |
| `EGoTouchRev/.../TouchSolver/PeakDetector.cpp` | 峰值检测参考 |
| `EGoTouchRev/.../TouchSolver/ZoneExpander.cpp` | 区域扩展 + 质心计算参考 |
| `EGoTouchRev/.../TouchSolver/EdgeCompensation.cpp` | 边缘补偿参考 |
| `EGoTouchRev/.../Reporting/TouchTracker.cpp` | 触点跟踪参考 |

---

## 数据结构设计

在 `himax_ts_data` 中新增指针 `struct hx_algo *algo`，在 probe 时通过 `devm_kzalloc` 一次性分配（~40KB，避免栈上分配）。

```c
/* 最大常量 */
#define HX_ROWS        40
#define HX_COLS        60
#define HX_PIXELS      (HX_ROWS * HX_COLS)  /* 2400 */
#define HX_MAX_ZONES   20
#define HX_MAX_PEAKS   20
#define HX_ZONE_PX_MAX 300  /* 单个 zone 最大像素数 */

struct hx_peak {
    u8 r, c;
    s16 z;               /* 信号强度 */
    s32 nbr_sum;          /* 8 邻域信号和 */
    u16 zone_area;        /* 所属 macro zone 面积 */
};

struct hx_contact {
    s32 x, y;             /* Q8.8 定点数，网格坐标 */
    u16 area;
    s32 signal_sum;
    bool is_edge;
};

struct hx_track {
    bool active;
    s32 x, y;             /* 输出坐标 0~65535 */
    s32 vx, vy;           /* 速度（输出坐标/帧） */
    s32 signal_sum;
    u8 age;               /* 存活帧数 */
    u8 missed;            /* 连续丢失帧数 */
    u8 debounce;          /* 剩余去抖帧数 */
};

struct hx_macro_zone {
    u16 pixels[HX_ZONE_PX_MAX]; /* 1D 索引 (r*60+c) */
    u16 area;
    s32 signal_sum;
    u8 min_r, max_r, min_c, max_c; /* 包围盒 */
};

struct hx_algo {
    /* ---- 帧缓冲 ---- */
    s16 frame[HX_ROWS][HX_COLS];        /* 4800B: 基线减除后的信号 */
    s16 iir_history[HX_ROWS][HX_COLS];  /* 4800B: IIR 历史帧 */
    bool iir_initialized;

    /* ---- 检测缓冲 ---- */
    u8  visited[HX_PIXELS];              /* 2400B: BFS visited */
    u8  zone_map[HX_PIXELS];             /* 2400B: 每像素 zone ID */
    u16 bfs_queue[HX_PIXELS];            /* 4800B: BFS 队列 */

    /* ---- 检测结果 ---- */
    struct hx_macro_zone zones[HX_MAX_ZONES];
    u8 zone_count;

    struct hx_peak peaks[HX_MAX_PEAKS];
    u8 peak_count;

    struct hx_contact contacts[HIMAX_MAX_TOUCH];
    u8 contact_count;

    /* ---- 跟踪状态 ---- */
    struct hx_track tracks[HIMAX_MAX_TOUCH];
    bool touch_active;
    u8 touch_start_frames;

    /* ---- sysfs 可调参数 ---- */
    /* 预处理 */
    bool cmf_enabled;               /* 默认 true */
    s16  cmf_exclusion;             /* 默认 250 */
    s16  cmf_max_correction;        /* 默认 500 */
    bool iir_enabled;               /* 默认 true */
    u16  iir_decay_weight;          /* 默认 200 (0~256) */
    u16  iir_decay_step;            /* 默认 80 */
    s16  iir_noise_floor;           /* 默认 5 */
    s16  iir_gate_floor;            /* 默认 200 */
    u8   iir_gate_ratio_q8;         /* 默认 26 (≈10%) */
    bool gauss_enabled;             /* 默认 false */
    u8   gauss_center_weight;       /* 默认 20 */
    /* 检测 */
    s16  macro_threshold;           /* 默认 800 */
    s16  peak_threshold;            /* 默认 800 */
    bool palm_enabled;              /* 默认 true */
    u8   palm_area_threshold;       /* 默认 50 */
    s32  palm_signal_threshold;     /* 默认 80000 */
    s16  palm_density_low;          /* 默认 400 */
    /* 跟踪 */
    s32  track_dist2_max;           /* 默认 (6*1092)^2 ≈ 42M (6 格) */
    u8   track_lost_frames;         /* 默认 3 */
    u8   debounce_base;             /* 默认 2 */
};
```

**内存预算**: ~40KB，在 probe 一次性分配，驱动生命周期内不再分配/释放。

---

## 处理流水线

```
himax_ts_thread() IRQ 处理:
  ┌─ 读取 SPI 原始帧 (保留现有逻辑)
  │
  ├─ Phase 1: 预处理
  │   ├─ 1A. 基线减除: raw - 0x7FFE → s16 frame[][]
  │   ├─ 1B. CMF 共模滤波 (默认启用, sysfs 可关)
  │   ├─ 1C. GridIIR 时域滤波 (默认启用, sysfs 可关)
  │   └─ 1D. 高斯滤波 (默认关闭, sysfs 可开)
  │
  ├─ Phase 2: 触点求解
  │   ├─ 2A. 宏区域检测 (8-连通 BFS)
  │   ├─ 2B. 掌压抑制 (面积/信号/密度/纵横比)
  │   ├─ 2C. 峰值检测 (非对称局部极值 + 多级过滤)
  │   └─ 2D. 区域扩展 + 加权质心 (Q8.8 定点)
  │
  ├─ Phase 3: 触点跟踪
  │   ├─ 3A. 贪心距离匹配 + 速度预测
  │   └─ 3B. input_mt 上报
  │
  └─ 结束
```

---

## Phase 1: 预处理

### 1A. 基线减除 (替换 `simple_filter` + `dd2d`)

```
算法: frame[r][c] = (s16)le16_to_cpu(raw[r*60+c]) - 0x7FFE
特殊: raw[0] 始终为 0x0000, 设 frame[0][0] = 0
复杂度: O(2400), 纯整数减法
```

替换现有 `EQUILIBRIUM`/`THRESHOLD`/`simple_filter`/`dd2d` 四处逻辑。基线值与 Windows 端对齐（0x7FFE=32766）。

### 1B. CMF 共模滤波 (**关键** — 解决充电器噪声)

参考: `CMFProcessor.cpp` DualDim 模式

```
行滤波:
  for each row:
    sum = 0, count = 0
    for each pixel in row:
      if abs(val) < exclusion_threshold(250):
        sum += val; count++
    offset = clamp(sum / count, -max_correction, +max_correction)
    for each pixel: val -= offset

列滤波: 同上，逐列处理

先行后列（DualDim）。
```

纯整数，无定点。每帧 O(4800) 加法 + 1 次除法/行（40 次）+ 1 次除法/列（60 次）。

### 1C. GridIIR 时域滤波 (**重要** — 降噪底噪)

参考: `GridIIRProcessor.cpp`

```
frame_max = max(abs(frame[r][c])) for all pixels
dyn_threshold = max(frame_max * gate_ratio_q8 / 256, gate_floor)

for each pixel:
  if current >= dyn_threshold:
    output = current              // 保护真实触控信号
  else:
    output = (decay_weight * current + (256 - decay_weight) * history) / 256
    output = max(0, output - decay_step)
    if output < noise_floor: output = 0
  frame[r][c] = output
  iir_history[r][c] = output
```

首帧将 frame 直接拷贝到 history 作初始化。全部整数运算（weight 是 0~256 的 Q0.8）。

### 1D. 高斯滤波 (**可选** — 默认关闭)

参考: `GaussianFilter.cpp`

```
3x3 核: [1,2,1; 2,W,2; 1,2,1], W = gauss_center_weight(20)
kernelSum = 12 + W
output[r][c] = convolution / kernelSum
边界像素不处理（保持原值）
```

纯整数。需一个临时缓冲（可复用 `bfs_queue` 的 4800B 空间作为 `s16 temp[2400]`）。

---

## Phase 2: 触点求解

### 2A. 宏区域检测 (替换全网格扫描)

参考: `MacroZoneDetector.cpp`

```
memset(visited, 0)
zone_count = 0
for r=0..39, c=0..59:
  if visited[r*60+c] || frame[r][c] < macro_threshold(800):
    continue
  // BFS 8-连通
  初始化 zone: min/max 包围盒, area=0, signal_sum=0
  enqueue (r,c)
  while queue not empty:
    dequeue (nr,nc)
    if visited || frame < threshold: skip
    mark visited
    zone.pixels[area++] = nr*60+nc (上限 HX_ZONE_PX_MAX=300)
    zone.signal_sum += frame[nr][nc]
    更新包围盒
    enqueue 8 个邻居
  zone_count++ (上限 HX_MAX_ZONES=20)
```

BFS 队列使用 `algo->bfs_queue[2400]`，环形缓冲（head/tail 索引）。无动态分配。

### 2B. 掌压抑制 (**关键** — 防误触)

参考: `PalmRejector.cpp`

四条判定规则（全部整数）:

| 规则 | 条件 | 说明 |
|---|---|---|
| 1 | `area >= palm_area_threshold(50)` | 大面积 → 手掌 |
| 2 | `signal_sum >= palm_signal_threshold(80000)` | 强信号积分 → 手掌/拳头 |
| 3 | `area >= 20 && signal_sum < palm_density_low * area` | 低密度（乘法代替除法） |
| 4 | `area >= 10 && max_side * 256 >= 1024 * min_side` | 纵横比 ≥ 4:1（定点乘法代替浮点除法） |

命中任一规则的 zone 从数组中移除（前移紧缩）。

### 2C. 峰值检测 (替换 `is_local_peak` + `raw2rxtx`)

参考: `PeakDetector.cpp`

```
遍历存活 zone 的每个像素:
  非对称局部极值判定:
    上+左邻域: 严格小于 (neighbor < center)
    下+右邻域: 允许等于 (neighbor <= center)
  → 消除极值方向歧义，减少重复检测

多级过滤（按顺序）:
  1. Z1 阈值: z < peak_threshold(800) → 拒绝
  2. Z8 隔离: (z >> 5) > nbr_sum → 拒绝（孤立尖峰）
  3. 压力漂移: 平坦手掌检测（行梯度分析，全整数）
  4. 边缘峰过滤: 同一边上弱峰 < 最强峰 * 5/8 → 拒绝
  5. Zone 最小面积: zone.area < 2 → 拒绝

结果存入 peaks[]，最多 20 个，按信号强度升序排列。
```

### 2D. 区域扩展 + 加权质心 (替换 `rxtx2xy`)

参考: `ZoneExpander.cpp`

```
对每个 peak:
  zone_thold = min(peak_threshold, peak.z) * 0x40 >> 7  (≈50%)
  
  BFS 从 peak 位置向外扩展:
    if frame[r][c] >= zone_thold && zone_map 未被占用:
      标记 zone_map
      累加加权质心:
        weighted_col += c * 128 * signal    (s64)
        weighted_row += r * 128 * signal    (s64)
        weight_total += signal              (s32)
        area++; signal_sum += signal

  质心计算 (Q8.8 定点网格坐标):
    cx_q8 = (weighted_col * 2 / weight_total + 0x80)
    cy_q8 = (weighted_row * 2 / weight_total + 0x80)

  多峰 zone: 若 BFS 遇到已分配给其他 peak 的像素，使用 3x3 局部质心

  缩放到输出坐标 [0, 65535]:
    output_x = (s64)cx_q8 * 65535 / (60 * 256)
    output_y = (s64)cy_q8 * 65535 / (40 * 256)

  超过 10 个 contact 时，保留 signal_sum 最大的 10 个
```

中间乘积使用 s64 防溢出（col=59, signal=32767, ×128 → 单像素 ~247M，300 像素累加 ~74G，需 s64）。

---

## Phase 3: 触点跟踪

### 3A. 贪心距离匹配 + 速度预测 (替换 `himax_track_contacts`)

参考: `TouchTracker.cpp`（使用贪心而非匈牙利）

```
1. 预测: pred_x = track.x + track.vx, pred_y = track.y + track.vy
2. 距离矩阵: dist2[c][t] = (contact.x - pred_x)^2 + (contact.y - pred_y)^2  (s64)
3. 贪心匹配:
   - 构建 (contact_idx, track_idx, dist2) 候选对
   - 按 dist2 升序排序
   - 依次匹配未使用的 contact/track 对
   - 阈值: track_dist2_max (默认 ~42M = 6 格距离²)

4. 匹配成功:
   track.vx = new_x - track.x
   track.vy = new_y - track.y
   track.x = new_x; track.y = new_y
   track.missed = 0; track.age++
   if debounce > 0: debounce--

5. 未匹配 track:
   missed++; if missed > track_lost_frames(3): 释放 slot

6. 未匹配 contact → 新 track:
   分配空闲 slot, age=1, debounce=debounce_base(2)
   弱信号拒绝: signal_sum < 55 && area < 3 → 不创建

7. 去抖: debounce > 0 时不上报该 slot（类似现有 NEW_TOUCH_DEBOUNCE）
```

### 3B. 上报 (保留 `himax_report_tracked_state` 结构)

- 适配新的 `hx_track` 结构
- 启用 `ABS_MT_TOUCH_MAJOR`: `min(isqrt(area) * 16, 255)`
- 压力: `clamp(signal_sum >> 2, 0, 4095)` 映射到 0~4095
- 保留 `touchscreen_report_pos` + `input_mt_report_pointer_emulation`

---

## Phase 4: sysfs 属性

通过 `DEVICE_ATTR_RW` 宏暴露运行时可调参数，注册为 `attribute_group`：

| 属性名 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| `cmf_enabled` | bool | 1 | CMF 开关 |
| `cmf_exclusion` | int | 250 | CMF 排除阈值 |
| `cmf_max_correction` | int | 500 | CMF 最大修正 |
| `iir_enabled` | bool | 1 | IIR 开关 |
| `iir_decay_weight` | int | 200 | IIR 混合权重 (0~256) |
| `iir_decay_step` | int | 80 | IIR 每帧衰减 |
| `gauss_enabled` | bool | 0 | 高斯开关 |
| `gauss_center_weight` | int | 20 | 高斯中心权重 |
| `palm_enabled` | bool | 1 | 掌压抑制开关 |
| `palm_area_threshold` | int | 50 | 掌压面积阈值 |
| `peak_threshold` | int | 800 | 峰值检测阈值 |
| `track_lost_frames` | int | 3 | 丢失容忍帧数 |

sysfs 写入无需加锁 — 参数为字长原子写，IRQ handler 读到新旧值均可接受。

---

## Phase 5: 驱动集成

### 修改 `himax_ts_data`:
- 移除: `struct himax_track tracks[10]`, `touch_start_frames`, `touch_active`
- 新增: `struct hx_algo *algo`

### 修改 `himax_spi_probe()`:
- 在现有分配后添加: `ts->algo = devm_kzalloc(dev, sizeof(*ts->algo), GFP_KERNEL)`
- 初始化所有默认参数
- 注册 sysfs 属性组: `devm_device_add_group()`

### 修改 `himax_input_dev_config()`:
- 取消注释 `ABS_MT_TOUCH_MAJOR` (0~255)
- 保留 `ABS_MT_PRESSURE` (0~4095)

### 修改 `himax_release_all_touches()`:
- 清空 `algo->tracks[]` 而非 `ts->tracks[]`
- 重置 `algo->iir_initialized = false`（resume 后重新初始化 IIR）
- 重置 `algo->touch_active`, `algo->touch_start_frames`

### 修改 `himax_ts_thread()`:
- 替换第 1276~1306 行为新流水线调用
- 保留前面的 SPI 读取和错误处理
- 保留后面的 mutex unlock 和返回

---

## 实施批次

### Batch 1 — 基础框架 + 预处理 (首先实现，可独立验证)
1. 定义 `struct hx_algo` 及所有子结构
2. probe 中分配并初始化默认值
3. 基线减除 (Phase 1A)
4. CMF (Phase 1B)
5. GridIIR (Phase 1C)
6. 简化版峰值检测（直接在 frame 上扫描，复用现有逻辑验证预处理效果）

**验证点**: 加载驱动后通过 evtest 确认基本触控可用，观察充电器接入时噪声是否改善。

### Batch 2 — 区域检测 + 掌压抑制
7. 宏区域 BFS (Phase 2A)
8. 掌压抑制 (Phase 2B)
9. 将峰值检测限制在 zone 内 (Phase 2C 完整版)

**验证点**: 手掌放上不再产生触控事件，多指触控正常。

### Batch 3 — 精度提升
10. 区域扩展 + 加权质心 (Phase 2D)
11. 速度预测跟踪器 (Phase 3A)
12. 启用 ABS_MT_TOUCH_MAJOR + pressure 映射 (Phase 3B)

**验证点**: 触控精度提升（尤其边缘区域），快速滑动时 ID 不跳变。

### Batch 4 — 打磨
13. sysfs 属性组 (Phase 4)
14. 高斯滤波可选启用 (Phase 1D)
15. 边缘峰值过滤细化
16. 压力漂移过滤器

**验证点**: 通过 sysfs 实时调参，确认各开关生效。

---

## 风险与注意事项

1. **栈空间**: `hx_algo` ~40KB 绝不能放栈上。必须 devm_kzalloc。threaded IRQ 栈仅 ~8KB。
2. **BFS 无动态分配**: 用静态环形缓冲 `bfs_queue[2400]`（head/tail 索引）。最大队列 = 全网格 2400，保证够用。
3. **Zone 像素上限**: `pixels[300]` 上限。超过 300 像素的 zone 几乎必定触发掌压抑制（area>50），上限安全。BFS 继续标记 visited 但不再存储索引。
4. **质心除法**: `weight_total == 0` 时退化到 peak 坐标。中间积用 s64。
5. **坐标空间**: 输出保持 [0, 65535]。`cx_q8 * 65535 / (60 * 256)` 用 s64 中间值。
6. **IIR 首帧**: `iir_initialized == false` 时直接拷贝当前帧到 history。resume 后自动重置。
7. **sysfs 竞态**: 参数为简单整型，字长原子访问，不需要加锁。

---

## 验证方法

1. **编译验证**: `make -C /lib/modules/$(uname -r)/build M=$(pwd)` 无 warning
2. **加载验证**: `insmod himax-spi.ko` + `dmesg` 无错误
3. **基本触控**: `evtest /dev/input/eventX` — 单指点触、滑动
4. **多指触控**: 2~5 指同时触控，验证 slot 分配和 ID 稳定
5. **掌压抑制**: 手掌放上不产生事件
6. **充电噪声**: 接入充电器后触控不漂移
7. **边缘触控**: 屏幕四边触控响应正常
8. **Suspend/Resume**: 合盖再开触控恢复正常
9. **sysfs 调参**: `echo 0 > /sys/devices/.../cmf_enabled` 确认开关生效
