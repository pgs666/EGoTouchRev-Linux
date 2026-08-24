# 驱动架构

本文描述当前代码的职责边界，供后续拆分和性能优化使用。它描述的是实现约束，不是
完整算法说明。

## 数据路径

```text
SPI IRQ
  -> event-stack transport and validation
  -> firmware finger-state decoding
  -> baseline / CMF / optional IIR preprocessing
  -> zones / palm / peaks / coordinates
  -> persistent peak identity and slot tracking
  -> Linux input Type-B reporting
```

无效、retry 或 SPI 失败帧不得进入算法，也不得推进 track 的 `missed`。运行时整线
噪声帧保持上一触点；只有连续异常达到阈值后，生命周期层才允许执行完整恢复。

## 生命周期

panel enable 默认执行完整 reset/reload 路径，并依次确认 MCU、firmware reload、raw
producer 和 wake raw quality。算法维护 `working`、`wake_candidate` 与 `last_safe`
三份 baseline；未通过质量判定的候选不能覆盖 safe baseline。

恢复流程与触点算法必须保持单向依赖：生命周期层可以重置算法瞬态状态，算法不能
直接访问 SPI 或发起硬件 reset。

## 诊断边界

`HIMAX_DIAGNOSTICS=1` 构建可加入 event stack、处理后矩阵和 track ring。普通构建
必须在预处理阶段裁掉这些结构与复制操作。轻量健康计数若用于自动恢复，可以保留在
普通构建，但不能伪装成诊断功能。

## 内存和并发

- 大型网格与 Hungarian scratch 均由 probe 分配，不能放在内核栈；
- IRQ thread、panel follower 和手动 reset 通过设备操作锁串行化；
- IRQ enable 状态由 spinlock 保护；
- sysfs 参数更新必须是单值原子写，复合 profile 应在用户态逐项应用；
- remove/shutdown 后不得重新排队恢复 work。

## 文件职责

- `himax-spi-core.c`：probe/remove、panel 生命周期、恢复协调、IRQ 和 Type-B 上报；
- `himax-spi-hw.c`：SPI、AHB、寄存器访问、safe mode、reload 和固件握手；
- `himax-spi-sysfs.c`：参数 ABI、手动 reset 节点和可选诊断 ring；
- `himax-spi.h`：仅供驱动编译单元共享的设备状态和内部接口；
- `hx-algo.c`：算法默认值、状态 reset 和 wake baseline qualification；
- `hx-algo-preprocess.c`：baseline、CMF、edge boost 和可选 IIR；
- `hx-algo-detect.c`：zone、palm 和 peak detection；
- `hx-algo-resolve.c`：contact-region expansion、centroid 和坐标求解；
- `hx-algo-track.c`：peak identity、assignment、slot 生命周期和坐标滤波；
- `hx-algo.h`：不依赖 SPI/input device 的算法状态与公开测试接口。

后续拆分不得以导出大量全局变量为代价。共享设备状态放在私有 header 的结构体中，跨
文件函数仅暴露完成一项职责的操作。
