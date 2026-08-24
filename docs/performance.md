# 性能审计

本文记录触控数据路径的性能边界、已经删除的冗余和仍需实机 A/B 验证的候选项。性能
修改不得以增加断触、启动延迟或丢失诊断证据为代价。

## 本轮已处理

- 默认关闭的 Grid IIR 不再每帧维护 2400 节点历史。运行时重新开启 IIR 时，第一帧
  只负责完整初始化历史，因此不会读取 suspend 前或禁用期间的旧数据。
- baseline 的 common-mode 统计同时记录最大 raw-to-baseline 差值。固件报告无手指时
  的矛盾峰判断不再额外遍历整张网格。
- 删除未产生 input event、未参与触点求解、也未进入诊断输出的内部手势状态机及其
  sysfs/tuner 参数。
- frame 处理返回值复用本轮 slot 遍历的 reported 计数，不再额外调用一次稳定轨迹
  计数。
- Hungarian scratch 只清理当前矩阵使用的行列；peak edge 判定复用已保存字段。
- live/full reset 不再清空由有效计数保护、或会在消费前自行覆盖的 scratch 数组。
- 仅供 host fixtures 使用的三个兼容 wrapper 不再进入正式内核模块符号表。

普通构建因此减少一次 4800-byte 每帧复制；固件 no-finger 路径减少一次 2400 节点
扫描。删除的手势状态机和重复 reported 计数各减少一次最多 10 slot 的每帧遍历。

## 有意保留

- `hx_algo_is_exception_frame()` 在 baseline 更新前检查整线/全屏电气噪声。虽然它会
  读取整张网格两次，但合并到 preprocessing 会先污染 working baseline，不能直接
  删除。
- zone detection 和 contact expansion 分别清空 `visited` 与 `zone_map`。两个数组
  表示不同阶段的所有权，合并会改变多峰区域的解析顺序。
- Type-B 上报遍历全部 slot，以确保 release 和 `INPUT_MT_DROP_UNUSED` 的状态一致。
- diagnostics 构建每帧保存 event stack、处理后网格、peak 和 track。这是诊断版的
  明确成本；普通构建由预处理宏完全裁掉这些结构、分配与复制。

## 后续候选（需实机 A/B）

1. SPI read 目前会清零整块 full-duplex TX/RX buffer，并在传输后复制 payload。可以
   评估同一 `spi_message` 内的 header transfer + payload transfer，让控制器生成 dummy
   bytes 并直接接收到目标缓冲；必须先确认 SPI 控制器保持片选且 Himax 协议接受这种
   transfer 边界。
2. baseline 已输出稀疏、common-mode 修正后的 frame，但默认 CMF 仍进行完整 row/column
   双向遍历。需要用现场 raw trace 对比关闭 CMF 后的弱触、充电噪声与角落信号，不能
   仅凭代码形态删除。
3. input 上报可评估 active/released slot 位图，减少无变化 slot 的调用；必须覆盖多指
   抬起、silent gap、invalid frame hold 和 suspend release。
4. 若 profiling 证明 histogram 清零占比明显，可改用 generation-tagged bins；当前
   2048-bin 的连续 `memset` 更简单，也更利于缓存预取，暂不复杂化。

## 验证要求

每次性能修改至少执行 `make check`、普通模块构建和 `HIMAX_DIAGNOSTICS=1` 构建。涉及
SPI、input 上报或 CMF 的候选还必须在 Linux 实机覆盖连续亮灭屏、快速边缘滑动、多指
交叉和噪声帧保持，再同步 Android。
