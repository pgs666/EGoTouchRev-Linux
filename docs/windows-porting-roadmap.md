# HX83121A 后续 Windows 特性移植路线图

本文记录 Gaokun Windows 原厂实现中，仍值得选择性移植到当前 Linux/Android
驱动的能力，并给出固定实施顺序。目标不是完整复刻 TSACore，而是在不破坏现有
亮屏稳定性的前提下，依次解决运行时失效、坐标抖动、快速滑动和复杂电气干扰。

参考资料位于本机分析仓库：

- `HuaweiThpService_RE/08_himax_drv_deepdive.md`
- `HuaweiThpService_RE/10_apdaemon_deepdive.md`
- `HuaweiThpService_RE/11_tsacore_deepdive.md`
- `HuaweiThpService_RE/12_thp_service_deepdive.md`

## 当前稳定基线

截至 2026-08-25，`codex/windows-resume-safebaseline` 分支已经实现：

- 亮屏时完整硬件 reset、MCU ready、firmware reload 和 raw producer 等待；
- 去除没有完成状态确认的自动 calibration；
- `last_safe / wake_candidate / working` 三份 baseline；
- 唤醒 raw-quality qualification 和异常 candidate 双窗口确认；
- 唤醒时已有手指的 baseline 保护；
- RX/TX 整线噪声帧保持上一触点；
- 连续 SPI 失败和持续电气噪声后的完整硬件恢复；
- persistent peak ID、split competition age、Hungarian peak-ID 代价和
  continuation-only 约束。

实机已经出现一次“亮屏后完全正常”的关键正向结果。后续修改必须以此版本为
可回退基线，不能同时重写唤醒、坐标和跟踪三个子系统。

## 固定实施顺序

### 1. 冻结并验证当前亮屏恢复基线

先不增加新算法，完成足够次数的实机回归：

- 冷启动；
- 连续亮屏/灭屏；
- 亮屏时手指已经放在屏幕上；
- 充电和未充电；
- 日常模式和游戏模式；
- 慢速画线、快速单指滑动和多指交叉。

每轮至少记录 resume 路径、wake qualification 结果、baseline 来源、reset 次数、
noise hold 次数和 slot/tracking ID 生命周期。

完成标准：连续多轮亮屏不再进入旧 retained-resume/calibration 路径，且没有因
wake qualification 引入明显的首触延迟或鬼触。若本阶段失败，应只修复亮屏闭环，
不要进入后续坐标优化。

### 2. 移植运行时帧流看门狗和分级恢复

Windows 的 `ThpBase::MainLoop`、`AbnormalRestart`、SOS 和 Himax 固件状态检查表明，
产品实现不会只依靠 SPI 返回值判断控制器健康。当前 Linux 仍应补充：

- `DEADFEED` 或等价固件跑飞状态识别；
- 连续 invalid/retry master frame 计数；
- 帧序号、事件栈 hash 或 raw 内容长期停滞检测；
- 固件 finger 状态与 raw 峰长期矛盾检测；
- reset/reload 后 MCU、firmware 和 raw producer 再次确认；
- 所有触发原因、阈值和恢复结果进入 diagnostics/trace。

恢复必须分级：单帧异常保持上一触点，短异常只计数，持续异常才关闭 IRQ 并执行
完整初始化。需要设置退避和频率限制，避免硬件持续异常时形成 reset storm。

完成标准：注入重复帧、invalid/retry 帧和 SPI 错误时不产生单帧 UP；持续异常能
自动恢复，并能从日志中唯一确认恢复原因和结果。

### 3. 建立 BOE/CSOT 原厂手指参数配置

先读取 panel/project ID，确认设备使用 BOE 还是 CSOT，再从
`GaokunHimaxBOE*.bin` 或 `GaokunHimaxCSOT*.bin` 提取与手指路径直接相关的参数：

- `fingerThold`、动态 peak threshold；
- `sigRatioToDirectReport`；
- `sigMultNormal / sigMultSD`；
- `accThold`；
- 四边独立 edge correction ratio；
- `cmfSigThold / cmfSigDiffThold`；
- charger noise threshold；
- touch-zone、split peak 和坐标求解相关参数。

不要把 6288 B 参数块直接解释为内核 ABI，也不要一次性照搬全部默认值。应建立
明确版本的 C profile，并保留现有 profile 作为回退。每个导入值都要记录原始
blob、偏移、含义和单位。

完成标准：profile 可按 panel ID 自动选择；未知面板回退到保守默认值；host test
验证所有参数范围，实机 A/B 不降低亮屏稳定性和边缘手势成功率。

### 4. 移植触摸区内的亚像素坐标求解

当前坐标主要来自扩展区域的 Q8.8 加权质心。它容易把节点级信号波动直接转换为
坐标抖动。Windows 手指路径还包含 `TZ_Process` 和 `ER_Process`，会在峰附近利用
相邻信号做二次曲线/端点拟合。

建议先实现一个有界、定点、可单测的 ER 子集：

- 仅对单一、紧凑且鞍点关系明确的触摸区启用；
- X/Y 分轴求亚像素偏移；
- 分母过小、邻点不单调或边界数据不足时回退现有质心；
- 输出限制在峰所在局部触摸区，禁止拟合产生跨区跳跃；
- 边缘使用独立修正规则，不虚构强信号节点。

不要先移植 DoubleReport/Bezier，也不要用更强的时间滤波掩盖错误坐标。空间求解
正确后，低速直线和圆形轨迹的台阶及抖动应先下降。

完成标准：固定 raw fixture 的坐标连续、不会越过触摸区；静止点、慢速横线、竖线、
斜线和圆形的逐帧残差均优于当前质心，快速滑动没有额外帧延迟。

### 5. 完善按速度变化的 X/Y 轨迹滤波

在亚像素求解稳定后，再调整时间域滤波。原则是低速强抑制、高速弱抑制，并对
X/Y 分轴处理：

- 静止和慢速绘制采用较低截止频率；
- 快速滑动提高跟随系数，避免手势起点后移和拖尾；
- 方向反转时限制过冲；
- silent-gap 后恢复时重置导数，不继承陈旧速度；
- 日常模式使用平滑 profile，游戏模式使用低延迟 profile。

可以继续使用当前定点 One Euro 框架，也可以参考 Windows 历史轨迹滤波，但不应
直接移植 DoubleReport。滤波只改变已拥有 slot 的坐标，不能参与触点存在性判断。

完成标准：圆形和斜线主观观感明显改善；高速移动误差、首触时间、抬手时间以及
slot ID 均不退化；游戏 profile 的新增延迟不可感知。

### 6. 依据新日志继续完善 peak identity ownership

当前已经部分移植 Windows `Peak_IDTracking/IDT` 思路。只有新内核仍出现以下现场时，
才继续本阶段：

- 单指产生第二个 Type-B slot；
- 原 slot 随后被释放；
- peak ID 在分裂的主峰/子峰之间发生 handoff；
- 没有 SPI、retry 或 baseline 异常可以解释该过程。

后续应实现“一组竞争峰只能拥有一个长期轨迹”的所有权模型，让瞬态 split child
不能抢走 established track，原长期峰也不能在 handoff 后立即创建新 slot。peak ID
应是连续性证据而不是绝对硬锁，以免高速运动时轨迹滞留在旧残余峰。

完成标准：单指 split fixture 全程只有一个 slot；双指靠近、交叉、分离仍保持两个
独立 tracking ID；不得用增加统一 debounce 的方式通过测试。

### 7. 扩展异常噪声分类和“保持上一触点”策略

在已有 RX/TX 整线检测上，按现场逐项增加：

- common-mode noise；
- charger/RX-line noise；
- signal disparity；
- raw jump；
- 局部真实触点与面状/整线干扰的空间区分。

每个分类先进入 diagnostics，只在误报率得到验证后才允许阻断正常求解。异常帧应
保持当前轨迹且不推进 `missed`；持续超时再交给第 2 阶段的恢复状态机。

完成标准：真实快速滑动不会被分类为噪声；注入的整线/面状干扰不会制造 UP、鬼触
或 baseline 污染。

### 8. 必要时扩展 SafeBaseline 队列

Windows 最多保存五份 safe baseline，并关联时间、频点和设备状态。当前固定形态
平板暂时只需要三份 baseline。只有日志证明存在多个可重复、稳定但彼此不同的显示
电气状态时，才扩展为 2～3 个 safe candidate：

- 保存创建时间、charger 状态、扫描频点和质量统计；
- 唤醒时用去除 common shift 后的空间差异选择候选；
- 有手指时只允许选择已提交 baseline，禁止学习新 candidate；
- 使用 LRU/质量淘汰，不能无界增长。

完成标准：两种已知显示状态能选择正确 baseline；错误选择会回退到 wake
qualification，而不是直接开始上报。

### 9. 最后评估动态跳频和扫描率管理

Windows 会根据噪声等级选择扫描频点和扫描率，但该能力依赖每频点 baseline、固件
命令完成状态和完整噪声判定。只有前八步稳定、且日志证明特定频点存在持续干扰时
才考虑：

- 完整实现 force-frequency/force-scan-rate 命令完成确认；
- 为各频点维护独立 baseline 或重新 qualification；
- 设置最小驻留时间和 hopping 次数限制；
- 触摸过程中切换不得产生 slot release 或明显采样间隔。

这是高风险可选项，不是当前断触和平滑问题的前置条件。

## 暂不移植

以下 Windows 能力目前与手指稳定性或平滑度收益不匹配：

- HPP2/HPP3 主动笔、蓝牙笔和笔校准；
- 工厂 open/short/raw/noise inspect 流程；
- 折叠屏、侧边音量键、电源键和 proximity 状态机；
- 完整 ApDaemon/TSACore 动态库与字符串命令框架；
- AFT 完整防边缘误触状态机；
- DoubleReport/Bezier 插帧；
- 无完成确认的自动 calibration。

AFT 可能降低从屏幕外开始的系统手势成功率，DoubleReport 可能改变游戏输入时序；
除非出现明确需求和可复现测试，否则不进入内核手指主路径。

## 每一步的通用交付要求

每一阶段必须独立提交，并满足：

1. 新逻辑具有编译期开关或运行时回退路径；
2. 增加对应 host regression fixture；
3. `make clean test` 和驱动全量构建通过；
4. 诊断字段能区分“逻辑未触发、触发成功、触发失败”；
5. 先部署 Linux 诊断版验证，再同步 Android 内核并部署指定 ESP slot；
6. A/B 时只改变一个子系统或一组明确参数；
7. 发现亮屏稳定性、slot 连续性或边缘手势退化时立即回退到当前稳定基线。

