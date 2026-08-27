# Windows 原版驱动提取与同步采集指南

本文是给 Windows 端协作 agent 的操作手册，目标是从 Gaokun 原版
`HuaweiThpService`/Himax 驱动取得**可验证的证据**，用于完善 Linux/Android
`HX83121A` baseline 和恢复流程。它不要求修改 Windows 系统，也不把社区
`EGoTouchRev-Windows` 的实现冒充原厂源码。

## 1. 证据优先级与输出目录

证据按可信度排序：

1. 同一次操作的动态原始数据（raw/mutual-cap、baseline、状态和时间戳）；
2. 原版 DLL 的反汇编/反编译及调用参数；
3. 导出表、字符串、参数 blob；
4. 社区重写或推测。第 4 类只能作为假设，不能直接移植。

建议每次采集建立唯一目录，例如 `capture\20260827-120000`，并保存：

```text
manifest.txt             文件 SHA-256、Windows 版本、触屏设备信息
raw-before/after.bin      原始网格或驱动返回的完整帧
baseline-before/after.bin working/safe/candidate baseline（若可取得）
state.csv                 每帧时间、epoch、BLSM/BLReset、异常原因
trace.etl 或 trace.txt    API/IOCTL 调用序列
logs/                     服务、驱动、TSACore 日志
```

文件名中不要使用“正常/故障”作为唯一标识；在 `manifest.txt` 记录明确的
操作时间线（亮屏、按下、松手、开始采集、断触时刻）。

## 2. 静态提取（Windows 命令行）

在副本上操作，先记录哈希：

```powershell
$out = "$env:USERPROFILE\Desktop\hx-extract-$(Get-Date -Format yyyyMMdd-HHmmss)"
New-Item -ItemType Directory -Force $out | Out-Null
$bins = Get-ChildItem "$env:WINDIR\System32\HuaweiThpService*",
  "$env:ProgramFiles\HuaweiThpService\*.dll" -ErrorAction SilentlyContinue
$bins | Get-FileHash -Algorithm SHA256 | Tee-Object "$out\hashes.txt"
$bins | Copy-Item -Destination $out -Force
systeminfo | Out-File "$out\systeminfo.txt"
```

若原版文件位于服务安装目录，应从 `HuaweiThpService.exe.config` 或服务注册表
项取得真实路径：

```powershell
Get-CimInstance Win32_Service |
  Where-Object {$_.Name -match 'Huawei|THP|Himax'} |
  Select Name,PathName,State | Tee-Object "$out\services.txt"
```

仓库中的 `peinfo.py`、`tools\pe_dump.py` 可直接复用。没有 Python 依赖时，
使用 Visual Studio 工具链：

```bat
dumpbin /exports TSACore.dll > TSACore.exports.txt
dumpbin /imports TSACore.dll > TSACore.imports.txt
dumpbin /headers TSACore.dll > TSACore.headers.txt
```

需要重点保存的静态对象：

- `TSACore.dll`：`BLIIR_*`、`BLSM_*`、`BLReset_*`、`SafeBaseline_*`、
  `AFT_ReleaseRecheck`、`Touch_KeepPrevTouchWithExceptionExcluded`；
- `himax_thp_drv.dll`：`thp_afe_start`、`hx_hw_reset`、`hx_set_nframe`、
  `hx_switch_mode`、`ahb_is_reload_done` 及 SPI/IOCTL 包装；
- `TSAPrmt.dll`：项目索引、动态参数 blob、CMF/频率/面板映射；
- `ApDaemon.dll`/`THP_Service.dll`：screen-on/off、休眠恢复和模式切换顺序。

反编译时同时导出函数地址、调用者和结构体偏移；单独保存伪代码，禁止只贴
截图。对每个结论标注 `exact`（反汇编可直接证明）或 `inferred`（根据行为推断）。

## 3. 动态采集：最小可复现实验

每轮只改变一个变量。推荐四轮：

1. 无触摸亮屏/灭屏；
2. 亮屏前保持一根手指；
3. 亮屏前保持手掌，亮屏后松手；
4. 正常快速滑动并记录断触时刻。

PowerShell 时间线记录模板：

```powershell
$t = [Diagnostics.Stopwatch]::StartNew()
"{0},screen_on" -f $t.ElapsedMilliseconds | Add-Content timeline.csv
# 执行亮屏/灭屏或人工操作后，分别记录 finger_down/finger_up/fault
```

如果厂商服务提供诊断开关，优先使用它导出完整帧；否则用 ETW/Procmon 仅抓
文件、注册表、IOCTL 和服务调用，不要拦截或改写 SPI 数据。Procmon 过滤建议：
进程为 `HuaweiThpService.exe`、`ApDaemon.exe`、相关 `svchost.exe`，路径包含
`Himax`/`THP`，操作包含 `DeviceIoControl`、`ReadFile`、`WriteFile`。

WinDbg（管理员）附加服务后可记录恢复顺序：

```text
bp himax_thp_drv!thp_afe_start
bp himax_thp_drv!hx_hw_reset
bp himax_thp_drv!ahb_is_reload_done
bp TSACore!BLReset_Check
bp TSACore!BLIIR_Reset
bp TSACore!AFT_ReleaseRecheck
bu kernel32!DeviceIoControl
g
```

每次断点命中保存 `k`, `r`, 参数内存（`db/dq`）和时间戳。若符号不可用，
使用静态导出地址加模块基址，并在报告中注明地址来源；不要猜测结构体字段。

## 4. 必须取得的数据字段

这是 Linux 当前仍缺少、最值得从原版提取的集合：

- 每帧 raw/mutual-cap 网格、帧号、retry/invalid 位、firmware `has_finger`；
- `working`、`safe`、`candidate` baseline 的快照、槽位 ID、epoch、创建/淘汰原因；
- BLSM 状态、计时器、`BLReset` 类型/原因/连续帧计数；
- 异常帧判定的节点数、全屏 common shift、负信号/charger RX-line 统计；
- AFE reset、reload、calibration start/done、raw type、N-frame、频率切换的顺序
  及等待时间；
- 有手指亮屏时是否冻结 baseline、何时开始 release recheck，以及最终提交哪一个
  baseline 槽位。

每个字段都要能与同一帧 raw 数据通过时间戳或帧号关联。只有日志文本而没有
原始网格时，只能判断调用顺序，不能证明 baseline 是否正确。

## 5. 可复用的解析片段

### 5.1 Python：按小端有符号 16 位解析网格

```python
from pathlib import Path
import struct, csv

def read_grid(path, tx, rx, offset=0):
    b = Path(path).read_bytes()[offset:offset + 2*tx*rx]
    if len(b) != 2*tx*rx:
        raise ValueError("short frame")
    return [struct.unpack_from('<h', b, 2*i)[0] for i in range(tx*rx)]

def diff(a, b):
    d = [y-x for x, y in zip(a, b)]
    return min(d), max(d), sum(abs(x) for x in d)/len(d)
```

### 5.2 C 风格：只在整帧成功后提交快照

```c
int capture_frame(struct hx_frame *dst)
{
    struct hx_frame tmp;
    int rc = spi_read_frame(&tmp);
    if (rc || !frame_valid(&tmp))
        return rc ? rc : -EBADMSG;
    *dst = tmp;                 /* 保留 last-good，不被失败帧覆盖 */
    return 0;
}
```

### 5.3 状态 CSV 最小格式

```text
mono_ms,frame,screen_epoch,blsm,blreset_reason,baseline_source,
exception,protected_contacts,queue_slot,raw_min,raw_max
```

## 6. 回传与审计

将整个采集目录压缩为 zip，并附上 `manifest.txt`、工具版本和复现实验步骤。
不要只回传截图或经过格式化的十六进制文本。涉及个人数据时删除触摸坐标之外
的系统日志；保留原始文件哈希，便于确认文件未被二次修改。

## 7. 当前明确的缺口

Linux event stack 没有 Windows 私有 touch-property、SideBaseline/SS buffer、
完整 CMF 参数和 calibration-done 握手。因此在拿到上述动态数据之前，不应继续
猜测 `BLReset_Check` 或增加更多 baseline 槽位；应先用 A/B 采集证明状态机的
进入、退出和槽位选择，再实现对应代码。

