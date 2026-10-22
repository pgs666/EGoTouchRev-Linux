# Linux TSACore 移植

此目录构建上游 EGoTouchRev v1.4.0 的完整、实际启用的手指算法。
上游以 Git submodule 固定在 `4d4f53d0bd8563d20850d5f380bcd6a8704c1e81`，
保留其许可证、参数表、模块边界和提交来源。这里不编译 Windows 服务、VHF、GUI 或
IPC，也不启用手写笔硬件采集；手指算法中的原版笔状态接口保留默认无笔状态。

架构为内核 SPI/电源管理及唯一 input device，加用户态完整 TSACore。
不把依赖浮点和进程级状态的 C++ 算法直接编入内核。

## 构建和离线验证

需要支持 C++23 的 GCC/Clang、CMake 和 Ninja。

```sh
git submodule update --init third_party/EGoTouchRev
cmake -S userspace -B userspace/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build userspace/build -j4
ctest --test-dir userspace/build --output-on-failure
userspace/build/himax-replay event-stack.bin
```

`himax-replay` 输入是连续的原生事件栈，默认每帧 5132 字节；
也支持显式指定 5060 字节 master 长度：

```sh
userspace/build/himax-replay frames.bin W273AS2700 5060
```

输出 CSV 坐标来自上游原版 `TsaOutputMapper`，单位为面板原始方向的显示像素；
时间以 120 Hz 重建，仅用于无时间戳裸帧回放。截断、校验失败和未知面板返回非零。
没有真实时间戳的回放不能用作实际输入延迟测量。

原生帧与 THP 帧之间只增加 3 字节运输头，不修改矩阵、suffix 或 checksum。
原生 master 的 checksum 从 offset 2 开始计算，覆盖至 5060 字节末尾；
与 THP 从 offset 5 开始计算等价。5132 字节运输记录末尾额外字节不参与该校验。
`0xbb` 由上游解释为校准完成，不能额外作为所有帧必须满足的固定魔数。

## 移植验收范围

以下是完整任务的要求，不以“库能编译”代替完整驱动验收：

1. 全部启用的 TSACore 模块、适配层、参数表及输出映射在 Linux 构建运行。
2. 内核提供版本化实时帧、生命周期 epoch 和受校验的结果回交；
   同一帧只有一个求解器上报，继续使用同一个 Type-B 输入设备。
3. AFE 状态确认、校准、扫描率/频点/IDLE 请求形成受限命令闭环；
   屏幕电源、复位和算法初始化的语义分别处理。
4. 守护进程停止、阻塞、设备解绑和 suspend/resume 不留下卡住的触点、
   越界结果、过期上报或悬空设备访问。
5. 录制/回放验证输入输出，覆盖静态/快速滑动/多指/边缘及生命周期；
   量化端到端时延。
6. Linux 服务安装、恢复方式、已知问题和实机验证结果可复现。

2026-10-22：第 1 项已有 Release 构建、合成状态转换及一份本机原生帧验证。
实时接口和硬件闭环仍在实现，尚不能把此目录称为完成部署的驱动。
旧内核算法在移植验收前保留，作为启动和故障恢复路径。
