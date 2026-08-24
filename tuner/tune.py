#!/usr/bin/env python3
"""EGoTouchRev-Linux 触屏算法调节 GUI"""

import glob
import os
import sys
import tkinter as tk
from tkinter import ttk, messagebox
from tkinter import font as tkfont
from pathlib import Path

try:
    from hidpi_tk import DPIAwareTk
    _TkBase = DPIAwareTk
except ImportError:
    _TkBase = tk.Tk

# ── 提权 ──────────────────────────────────────────────────────────────────────
if os.getuid() != 0:
    try:
        # pkexec 会剥离显示环境变量，需通过 `pkexec env` 透传
        passthrough = [
            "DISPLAY", "XAUTHORITY",           # X11
            "WAYLAND_DISPLAY", "XDG_RUNTIME_DIR",  # Wayland
        ]
        env_args = [f"{k}={v}" for k in passthrough
                    if (v := os.environ.get(k)) is not None]
        os.execvp("pkexec", [
            "pkexec", "env", *env_args,
            sys.executable, os.path.abspath(__file__),
            *sys.argv[1:],
        ])
    except OSError:
        pass  # pkexec 不可用，继续以当前权限运行

# ── 本地化 ────────────────────────────────────────────────────────────────────
_TR = {
    # 界面
    "app_title":             "EGoTouchRev-Linux Algo Tuner",
    "mode_label":            "使用模式",
    "mode_daily":            "日用模式",
    "mode_game":             "游戏模式",
    "tab_preprocessing":     "预处理",
    "tab_detection":         "检测",
    "tab_edge":              "边缘补偿",
    "tab_tracking":          "追踪",
    "btn_refresh":           "重新读取设备参数",
    "hint_entry":            "↵ 提交",
    "hint_game":             "游戏模式：已关闭轨迹平滑 / 已启用跳点检测",
    "hint_daily":            "日用模式：已恢复默认轨迹参数",
    # 错误
    "err_no_algo":           (
        "未找到驱动 sysfs 节点\n\n"
        "请确认 himax-spi 模块已加载。\n"
        "驱动版本可能不匹配。"
    ),
    "err_perm":              "写入 {name} 失败：权限不足，请以 root 运行",
    "err_generic":           "写入 {name} 失败：{err}",
    "err_invalid":           "无效值：「{val}」不是整数",
    # 参数标签
    "baseline_enabled":       "自适应基线",
    "baseline_noise_deadband":"基线噪声死区",
    "baseline_peak_threshold":"基线冻结阈值",
    "baseline_release_hold_frames": "基线释放保持帧",
    "cmf_enabled":           "CMF 开关",
    "cmf_exclusion":         "CMF 排除阈值",
    "cmf_max_correction":    "CMF 最大修正量",
    "iir_enabled":           "IIR 开关",
    "iir_decay_weight":      "IIR 衰减权重",
    "iir_decay_step":        "IIR 衰减步长",
    "iir_noise_floor":       "IIR 噪声底",
    "iir_gate_floor":        "IIR 门限底",
    "iir_gate_ratio_q8":     "IIR 门限比率 (Q8)",
    "macro_threshold":       "宏区阈值",
    "peak_threshold":        "峰值阈值",
    "palm_enabled":          "手掌拒绝",
    "palm_area_threshold":   "手掌面积阈值",
    "palm_signal_threshold": "手掌信号阈值",
    "palm_density_low":      "手掌低密度阈值",
    "edge_comp_enabled":     "边缘补偿开关",
    "edge_boost_pct":        "边缘信号增强 (%)",
    "edge_push_q8":          "边缘外推量 (Q8.8)",
    "edge_blend_q8":         "边缘混合范围 (Q8.8)",
    "track_dist2_max":       "最大匹配距离\u00b2",
    "track_lost_frames":     "允许丢失帧数",
    "debounce_base":         "防抖基准帧",
    "track_smoothing":       "轨迹平滑",
    "track_active_guard":    "活动保护",
    "track_start_debounce":  "起始防抖帧",
    "track_jump_dist2":      "跳点检测距离\u00b2",
    "baseline_background_alpha_shift": "背景基线更新速率",
    "baseline_no_finger_alpha_shift":  "无指基线更新速率",
    "baseline_recovery_alpha_shift":   "恢复基线更新速率",
    "baseline_background_max_step":    "背景基线最大步长",
    "baseline_no_finger_max_step":     "无指基线最大步长",
    "baseline_recovery_max_step":      "恢复基线最大步长",
    "baseline_recovery_max_frames":    "恢复状态最大帧数",
    "baseline_noise_tracking":         "基线噪声跟踪",
    "peak_local_radius":               "峰值局部半径",
    "peak_z8_enabled":                 "峰值 Z8 检测",
    "peak_saddle_enabled":             "鞍点检测",
    "peak_saddle_radius":              "鞍点检测半径",
    "peak_saddle_drop":                "鞍点下降阈值",
    "peak_signal_threshold_limit":     "压力漂移信号上限",
    "peak_edge_threshold":             "峰值边缘阈值",
    "peak_macro_min_area":             "峰值宏区最小面积",
    "palm_box_enabled":                "手掌框拒绝",
    "palm_box_expand_rows":            "手掌框扩展行数",
    "palm_box_expand_cols":            "手掌框扩展列数",
    "palm_box_match_distance":         "手掌框匹配距离",
    "palm_box_max_hold":               "手掌框保持帧数",
    "zone_cleanup_enabled":            "区域清理",
    "zone_max_radius":                 "区域最大半径",
    "zone_threshold_numer":            "区域阈值分子",
    "zone_threshold_shift":            "区域阈值移位",
    "edge_reject_enabled":             "边缘拒绝",
    "edge_reject_margin":              "边缘拒绝边距",
    "edge_reject_min_signal":          "边缘拒绝最小信号",
    "hungarian_enabled":               "匈牙利匹配",
    "debounce_weak_extra":             "弱信号防抖附加帧",
    "debounce_edge_extra":             "边缘防抖附加帧",
    "debounce_strong_signal":          "强信号阈值",
    "ghost_enabled":                   "鬼点过滤",
    "ghost_row_distance":              "鬼点行间距",
    "ghost_weak_ratio_q8":             "鬼点弱信号比率 (Q8)",
    "ghost_min_col_distance":          "鬼点最小列间距",
    "euro_enabled":                    "速度自适应平滑",
    "euro_alpha_min_q8":               "Euro 速率下限 (Q8)",
    "euro_alpha_max_q8":               "Euro 速率上限 (Q8)",
    "euro_speed_threshold":            "Euro 速度阈值",
    "gesture_drag_distance":           "拖拽手势距离",
    "gesture_long_press_frames":       "长按手势帧数",
}

# 参数说明文本（浅色小字）
_DESC = {
    "baseline_enabled":       "逐像素追踪温漂与 VCOM 漂移，触摸区域自动冻结",
    "baseline_noise_deadband":"基线附近按噪声慢速跟踪的范围",
    "baseline_peak_threshold":"高于此差值视为真实触摸并冻结该像素基线",
    "baseline_release_hold_frames": "抬手后暂缓吸收负回弹，避免产生假断触",
    "cmf_enabled":           "消除帧间共模噪声，改善基线稳定性",
    "cmf_exclusion":         "信号高于此值的像素不计入 CMF 均值，避免触点污染基线",
    "cmf_max_correction":    "每行/列 CMF 修正量的上限，防止过度补偿",
    "iir_enabled":           "IIR 时域滤波，平滑帧间信号抖动",
    "iir_decay_weight":      "与历史帧的混合权重（0 = 全历史，256 = 不混合）",
    "iir_decay_step":        "每帧信号的自然衰减量，抬手后加速清零",
    "iir_noise_floor":       "低于此值的信号直接清零，抑制静态噪声",
    "iir_gate_floor":        "动态门限的最低值，防止阈值过低",
    "iir_gate_ratio_q8":     "动态门限 = 帧最大值 \u00d7 ratio / 256",
    "macro_threshold":       "BFS 种子像素的最低信号值，越高越难触发",
    "peak_threshold":        "峰值候选的最低信号，过滤微弱触点",
    "palm_enabled":          "过滤面积或密度异常的大面积接触",
    "palm_area_threshold":   "像素数 \u2265 此值则判定为手掌",
    "palm_signal_threshold": "信号总和 \u2265 此值则判定为手掌",
    "palm_density_low":      "信号/面积 < 此值则判定为低密度手掌",
    "edge_comp_enabled":     "对边缘像素做信号增强与坐标外推，改善边缘触控精度",
    "edge_boost_pct":        "边缘像素信号放大百分比（50 = 放大 1.5\u00d7）",
    "edge_push_q8":          "质心向外最大推移量，Q8.8 定点数（128 = 0.5 格）",
    "edge_blend_q8":         "坐标混合过渡区宽度，Q8.8 定点数（512 = 2 格）",
    "track_dist2_max":       "帧间轨迹匹配的最大欧氏距离\u00b2，超出则新建轨迹",
    "track_lost_frames":     "连续丢失多少帧后释放轨迹槽",
    "debounce_base":         "新触点正式上报前须连续稳定的帧数",
    "track_smoothing":       "基于速度预测的坐标平滑，降低抖动（游戏模式建议关闭）",
    "track_active_guard":    "首次稳定触点确认前清除游离轨迹，防误触",
    "track_start_debounce":  "touch_active 确认所需帧数（0 = 立即，游戏模式建议为 0）",
    "track_jump_dist2":      "坐标突变距离\u00b2，超出则强制抬起再按下（0 = 禁用）",
    "baseline_background_alpha_shift": "冻结触摸格跟随全局共模变化的速率（移位越大越慢）",
    "baseline_no_finger_alpha_shift":  "无手指状态下基线更新平滑系数",
    "baseline_recovery_alpha_shift":   "检测到手指但尚无冻结格时的基线恢复速率",
    "baseline_background_max_step":    "背景状态下基线单帧最大修正量",
    "baseline_no_finger_max_step":     "无手指状态下基线单帧最大修正量",
    "baseline_recovery_max_step":      "恢复状态下基线单帧最大修正量",
    "baseline_recovery_max_frames":    "无冻结格恢复状态的最长持续帧数",
    "baseline_noise_tracking":         "允许死区内以极小步长跟踪基线漂移",
    "peak_local_radius":               "局部极大值搜索邻域半径，抑制相邻毛刺",
    "peak_z8_enabled":                 "过滤缺少八邻域支撑的孤立尖峰",
    "peak_saddle_enabled":             "检测两峰间鞍点以分离邻近触点",
    "peak_saddle_radius":              "鞍点搜索邻域半径",
    "peak_saddle_drop":                "鞍点与峰值的最小落差，低于此值不拆分",
    "peak_signal_threshold_limit":     "压力漂移检测使用的独立信号阈值基准",
    "peak_edge_threshold":             "峰值靠近面板边缘时的信号门槛",
    "peak_macro_min_area":             "峰值区域最小像素数，过滤孤立噪点",
    "palm_box_enabled":                "以包围盒扩展识别大面积手掌接触",
    "palm_box_expand_rows":            "手掌框在行方向向外扩展的像素数",
    "palm_box_expand_cols":            "手掌框在列方向向外扩展的像素数",
    "palm_box_match_distance":         "相邻帧手掌框匹配的最大距离",
    "palm_box_max_hold":               "手掌框无匹配时的最长保持帧数（0 = 禁用超时）",
    "zone_cleanup_enabled":            "对触点周围区域做形态学清理，去除杂散像素",
    "zone_max_radius":                 "区域清理的搜索半径",
    "zone_threshold_numer":            "区域阈值的分子（与移位共同构成阈值）",
    "zone_threshold_shift":            "区域阈值右移位数（基准 × numer >> shift）",
    "edge_reject_enabled":             "拒绝落在面板边缘且信号不足的触点",
    "edge_reject_margin":              "判定为边缘区域的内缩边距",
    "edge_reject_min_signal":          "边缘触点被保留所需的最小信号",
    "hungarian_enabled":               "用匈牙利算法做全局最优轨迹匹配",
    "debounce_weak_extra":             "弱信号触点额外需要的稳定帧数",
    "debounce_edge_extra":             "边缘触点额外需要的稳定帧数",
    "debounce_strong_signal":          "信号高于此值视为强触点，减少防抖帧数",
    "ghost_enabled":                   "过滤双指对向滑动时产生的鬼点",
    "ghost_row_distance":              "鬼点判定允许的最大行方向距离",
    "ghost_weak_ratio_q8":             "鬼点弱信号比率阈值 (Q8)，低于此比率视为鬼点",
    "ghost_min_col_distance":          "鬼点判定的列方向最小间距",
    "euro_enabled":                    "启用速度自适应坐标平滑（游戏模式建议关闭）",
    "euro_alpha_min_q8":               "低速移动时的平滑系数下限 (Q8)",
    "euro_alpha_max_q8":               "高速移动时的平滑系数上限 (Q8)",
    "euro_speed_threshold":            "达到最大平滑系数所需的移动速度",
    "gesture_drag_distance":           "判定为拖拽手势的最小移动距离",
    "gesture_long_press_frames":       "判定为长按手势的持续帧数",
}

def t(key, **kwargs):
    s = _TR.get(key, key)
    return s.format(**kwargs) if kwargs else s

def desc(name):
    return _DESC.get(name, "")


# ── 默认值（来自 hx_algo_init_defaults）──────────────────────────────────────
DEFAULTS = {
    "baseline_enabled":       1,
    "baseline_noise_deadband": 90,
    "baseline_peak_threshold": 305,
    "baseline_release_hold_frames": 60,
    "cmf_enabled":           1,
    "cmf_exclusion":         2000,
    "cmf_max_correction":    2000,
    "iir_enabled":           0,
    "iir_decay_weight":      200,
    "iir_decay_step":        80,
    "iir_noise_floor":       5,
    "iir_gate_floor":        200,
    "iir_gate_ratio_q8":     26,
    "macro_threshold":       280,
    "peak_threshold":        280,
    "palm_enabled":          1,
    "palm_area_threshold":   50,
    "palm_signal_threshold": 80000,
    "palm_density_low":      400,
    "edge_comp_enabled":     1,
    "edge_boost_pct":        50,
    "edge_push_q8":          128,
    "edge_blend_q8":         512,
    "track_dist2_max":       176400,  # 420 * 420
    "track_lost_frames":     4,
    "debounce_base":         1,
    "track_smoothing":       1,
    "track_active_guard":    1,
    "track_start_debounce":  1,
    "track_jump_dist2":      0,
    "baseline_background_alpha_shift": 3,
    "baseline_no_finger_alpha_shift":  3,
    "baseline_recovery_alpha_shift":   2,
    "baseline_background_max_step":  512,
    "baseline_no_finger_max_step":   512,
    "baseline_recovery_max_step":    256,
    "baseline_recovery_max_frames":   30,
    "baseline_noise_tracking":        1,
    "peak_local_radius":              1,
    "peak_z8_enabled":                1,
    "peak_saddle_enabled":            1,
    "peak_saddle_radius":             2,
    "peak_saddle_drop":              80,
    "peak_signal_threshold_limit":  1000,
    "peak_edge_threshold":          300,
    "peak_macro_min_area":            3,
    "palm_box_enabled":               1,
    "palm_box_expand_rows":           9,
    "palm_box_expand_cols":          10,
    "palm_box_match_distance":        6,
    "palm_box_max_hold":              0,
    "zone_cleanup_enabled":           1,
    "zone_max_radius":                3,
    "zone_threshold_numer":          64,
    "zone_threshold_shift":           7,
    "edge_reject_enabled":            1,
    "edge_reject_margin":            24,
    "edge_reject_min_signal":       500,
    "hungarian_enabled":              1,
    "debounce_weak_extra":            1,
    "debounce_edge_extra":            1,
    "debounce_strong_signal":      3000,
    "ghost_enabled":                  1,
    "ghost_row_distance":            32,
    "ghost_weak_ratio_q8":           96,
    "ghost_min_col_distance":       300,
    "euro_enabled":                   1,
    "euro_alpha_min_q8":             64,
    "euro_alpha_max_q8":            224,
    "euro_speed_threshold":          24,
    "gesture_drag_distance":         80,
    "gesture_long_press_frames":     45,
}

# 游戏预设（来自 game_preset.sh）
GAME_PRESET  = {
    "track_smoothing":      0,
    "track_start_debounce": 0,
    "track_jump_dist2":     6400,
    "euro_enabled":         0,
    "debounce_base":        0,
    "debounce_weak_extra":  0,
    "debounce_edge_extra":  0,
    "hungarian_enabled":    1,
    "ghost_enabled":        1,
}
# 日用模式恢复值（从 DEFAULTS 取）
DAILY_PRESET = {k: DEFAULTS[k] for k in GAME_PRESET}


# ── sysfs 定位 ────────────────────────────────────────────────────────────────
_KNOWN_PATH = Path(
    '/sys/devices/platform/soc@0/9c0000.geniqup/998000.spi'
    '/spi_master/spi0/spi0.0/algo'
)

def find_algo_dir():
    if _KNOWN_PATH.is_dir():
        return _KNOWN_PATH
    hits = glob.glob("/sys/devices/**/algo/peak_threshold", recursive=True)
    if hits:
        return Path(hits[0]).parent
    return None


# ── sysfs 读写（ALGO 由 App.__init__ 设置）───────────────────────────────────
ALGO = None

def read_param(name):
    try:
        return int((ALGO / name).read_text().strip())
    except Exception:
        return DEFAULTS.get(name, 0)

def write_param(name, value):
    try:
        (ALGO / name).write_text(str(value))
    except PermissionError:
        messagebox.showerror("权限错误", t("err_perm", name=name))
    except Exception as e:
        messagebox.showerror("错误", t("err_generic", name=name, err=e))


# ── Tab 参数定义 ───────────────────────────────────────────────────────────────
# 每条为 ("toggle", name)
#       ("slider", name, lo, hi, resolution)
#       ("entry",  name)
TABS = [
    ("tab_preprocessing", [
        ("toggle", "baseline_enabled"),
        ("slider", "baseline_noise_deadband", 0, 200, 1),
        ("slider", "baseline_peak_threshold", 100, 1000, 5),
        ("slider", "baseline_release_hold_frames", 0, 120, 1),
        ("toggle", "cmf_enabled"),
        ("slider", "cmf_exclusion",          50,  3000,  10),
        ("slider", "cmf_max_correction",     50,  3000,  10),
        ("toggle", "iir_enabled"),
        ("slider", "iir_decay_weight",        0,   256,   1),
        ("slider", "iir_decay_step",          0,   200,   1),
        ("entry",  "iir_noise_floor"),
        ("slider", "iir_gate_floor",         20,   500,   5),
        ("entry",  "iir_gate_ratio_q8"),
        ("slider", "baseline_background_alpha_shift", 0,   8,   1),
        ("slider", "baseline_no_finger_alpha_shift",  0,   8,   1),
        ("slider", "baseline_recovery_alpha_shift",   0,   8,   1),
        ("slider", "baseline_background_max_step",    0, 1024,  32),
        ("slider", "baseline_no_finger_max_step",     0, 1024,  32),
        ("slider", "baseline_recovery_max_step",      0,  512,  32),
        ("slider", "baseline_recovery_max_frames",    0,  120,   1),
        ("toggle", "baseline_noise_tracking"),
    ]),
    ("tab_detection", [
        ("slider", "macro_threshold",       100,  2000,  10),
        ("slider", "peak_threshold",        100,  2000,  10),
        ("toggle", "palm_enabled"),
        ("slider", "palm_area_threshold",     5,   150,   1),
        ("entry",  "palm_signal_threshold"),
        ("slider", "palm_density_low",       50,   800,  10),
        ("slider", "peak_local_radius",       1,     5,   1),
        ("toggle", "peak_z8_enabled"),
        ("toggle", "peak_saddle_enabled"),
        ("slider", "peak_saddle_radius",      1,     8,   1),
        ("slider", "peak_saddle_drop",        0,   256,   8),
        ("slider", "peak_signal_threshold_limit", 100, 2000, 10),
        ("slider", "peak_edge_threshold",   100,  1000,  10),
        ("slider", "peak_macro_min_area",     1,    20,   1),
        ("toggle", "palm_box_enabled"),
        ("slider", "palm_box_expand_rows",    0,    10,   1),
        ("slider", "palm_box_expand_cols",    0,    10,   1),
        ("slider", "palm_box_match_distance", 0,    20,   1),
        ("slider", "palm_box_max_hold",       0,    60,   1),
        ("toggle", "zone_cleanup_enabled"),
        ("slider", "zone_max_radius",         0,    16,   1),
        ("slider", "zone_threshold_numer",    0,   255,   1),
        ("slider", "zone_threshold_shift",    0,    12,   1),
    ]),
    ("tab_edge", [
        ("toggle", "edge_comp_enabled"),
        ("slider", "edge_boost_pct",          0,   200,   5),
        ("slider", "edge_push_q8",            0,   384,   8),
        ("slider", "edge_blend_q8",          64,  1024,  16),
        ("toggle", "edge_reject_enabled"),
        ("slider", "edge_reject_margin",      0,    64,   1),
        ("slider", "edge_reject_min_signal",  0,  2000,  50),
    ]),
    ("tab_tracking", [
        ("toggle", "track_smoothing"),
        ("toggle", "track_active_guard"),
        ("slider", "track_lost_frames",       1,    10,   1),
        ("slider", "debounce_base",           0,     8,   1),
        ("slider", "track_start_debounce",    0,     8,   1),
        ("entry",  "track_dist2_max"),
        ("entry",  "track_jump_dist2"),
        ("toggle", "hungarian_enabled"),
        ("slider", "debounce_weak_extra",     0,     8,   1),
        ("slider", "debounce_edge_extra",     0,     8,   1),
        ("slider", "debounce_strong_signal",  0,  8000, 100),
        ("toggle", "ghost_enabled"),
        ("slider", "ghost_row_distance",      0,    64,   1),
        ("slider", "ghost_weak_ratio_q8",     0,   255,   1),
        ("slider", "ghost_min_col_distance",  0,   800,  10),
        ("toggle", "euro_enabled"),
        ("slider", "euro_alpha_min_q8",       1,   255,   1),
        ("slider", "euro_alpha_max_q8",       1,   255,   1),
        ("slider", "euro_speed_threshold",    1,    64,   1),
        ("slider", "gesture_drag_distance",   0,   256,   4),
        ("slider", "gesture_long_press_frames", 1, 120,   1),
    ]),
]


# ── 可滚动 Tab 容器 ───────────────────────────────────────────────────────────
def _theme_bg():
    """返回当前主题的背景色。
    sv-ttk 不通过 style.lookup 暴露颜色，直接用 Tcl 读主题名判断。"""
    theme = ttk.Style().theme_use()
    if theme == "sun-valley-light":
        return "#fafafa"
    if theme == "sun-valley-dark":
        return "#1c1c1c"
    # clam / 其他主题：从 TFrame 读，拿不到就用系统默认
    return ttk.Style().lookup("TFrame", "background") or "#f0f0f0"


class ScrollableFrame(ttk.Frame):
    """带垂直滚动条的容器；内部控件放在 self.inner 里。"""
    def __init__(self, parent, **kw):
        super().__init__(parent, **kw)
        self.columnconfigure(0, weight=1)
        self.rowconfigure(0, weight=1)

        bg = _theme_bg()
        canvas = tk.Canvas(self, highlightthickness=0, bg=bg)
        sb = ttk.Scrollbar(self, orient="vertical", command=canvas.yview)
        canvas.configure(yscrollcommand=sb.set)

        canvas.grid(row=0, column=0, sticky="nsew")
        sb.grid(row=0, column=1, sticky="ns")

        self.inner = ttk.Frame(canvas, padding=(10, 8))
        self._win_id = canvas.create_window((0, 0), window=self.inner, anchor="nw")

        self.inner.bind("<Configure>",
                        lambda e: canvas.configure(
                            scrollregion=canvas.bbox("all")))
        canvas.bind("<Configure>",
                    lambda e: canvas.itemconfigure(
                        self._win_id, width=e.width))

        # 鼠标滚轮
        for w in (canvas, self.inner):
            w.bind("<MouseWheel>",
                   lambda e: canvas.yview_scroll(-1 * (e.delta // 120), "units"))
            w.bind("<Button-4>",
                   lambda e: canvas.yview_scroll(-1, "units"))
            w.bind("<Button-5>",
                   lambda e: canvas.yview_scroll(1, "units"))


# ── App ───────────────────────────────────────────────────────────────────────
class App(_TkBase):
    def __init__(self):
        super().__init__()   # DPIAwareTk.__init__ 会调用 fix_HiDPI
        self.title(t("app_title"))
        self.resizable(True, True)

        # 优先使用 Sun Valley 主题，不可用时回退到 clam
        try:
            import sv_ttk
            sv_ttk.set_theme("light")
            self._toggle_style = "Switch.TCheckbutton"
        except ImportError:
            ttk.Style(self).theme_use("clam")
            self._toggle_style = ""

        # 根窗口背景与主题同步，并通过 option_add 影响所有 tk 原生控件
        # （Canvas、ScrollableFrame 内部 canvas 等都靠这里统一）
        bg = _theme_bg()
        self.configure(bg=bg)
        self.option_add("*Background", bg)
        self.option_add("*background", bg)

        # clam（sv-ttk 父主题）对 TLabel/TFrame 有显式灰色背景，sv-ttk 未覆盖。
        # 手动对所有用到的 ttk 样式补上正确的背景色。
        _style = ttk.Style(self)
        for widget_style in (".", "TFrame", "TLabel", "TLabelframe",
                             "TLabelframe.Label", "TNotebook", "TNotebook.Tab"):
            _style.configure(widget_style, background=bg)

        # 字体：hidpi_tk 已处理 tk scaling，用 winfo_fpixels 读取实际 DPI 算字号
        self.update_idletasks()
        scale = max(1.0, self.winfo_fpixels('1i') / 96.0)
        sz       = max(9,  round(10 * scale))
        sz_small = max(8,  round(9  * scale))
        self._font       = tkfont.Font(family="Sans",      size=sz)
        self._font_small = tkfont.Font(family="Sans",      size=sz_small)
        self._font_mono  = tkfont.Font(family="Monospace", size=sz_small)

        # 定位 sysfs
        global ALGO
        ALGO = find_algo_dir()
        if ALGO is None:
            self._show_fatal()
            return

        self._vars = {}     # name -> tk.IntVar | tk.StringVar
        self._build()

        # 布局完成后设置初始尺寸和最小尺寸
        self.update_idletasks()
        w = max(680, self.winfo_reqwidth())
        h = max(720, min(self.winfo_reqheight(),
                         int(self.winfo_screenheight() * 0.85)))
        self.geometry(f"{w}x{h}")
        self.minsize(560, 380)

    # ── 错误界面 ──────────────────────────────────────────────────────────────
    def _show_fatal(self):
        self.resizable(False, False)
        frame = ttk.Frame(self, padding=32)
        frame.pack()
        ttk.Label(frame, text=t("err_no_algo"), justify="center",
                  font=self._font).pack()

    # ── 主界面 ────────────────────────────────────────────────────────────────
    def _build(self):
        # sysfs 路径提示
        hdr = ttk.Frame(self, padding=(10, 4, 10, 0))
        hdr.pack(fill="x")
        ttk.Label(hdr, text=str(ALGO), font=self._font_mono,
                  foreground="#888").pack(anchor="w")

        # 模式选择
        mode_lf = ttk.LabelFrame(self, text=t("mode_label"), padding=(12, 4))
        mode_lf.pack(fill="x", padx=10, pady=(6, 4))

        self._mode = tk.StringVar(value=self._detect_mode())
        ttk.Radiobutton(
            mode_lf, text=t("mode_daily"),
            variable=self._mode, value="daily",
            command=self._on_mode,
        ).pack(side="left", padx=12)
        ttk.Radiobutton(
            mode_lf, text=t("mode_game"),
            variable=self._mode, value="game",
            command=self._on_mode,
        ).pack(side="left", padx=12)
        self._mode_hint = ttk.Label(
            mode_lf, text=t("hint_game") if self._mode.get() == "game" else "",
            foreground="#666", font=self._font_small,
        )
        self._mode_hint.pack(side="left", padx=16)

        # Notebook（tabs 内容放在可滚动容器里）
        nb = ttk.Notebook(self)
        nb.pack(fill="both", expand=True, padx=10, pady=4)

        for tab_key, controls in TABS:
            sf = ScrollableFrame(nb)
            nb.add(sf, text=t(tab_key))
            self._build_tab(sf.inner, controls)

        # 底栏
        bottom = ttk.Frame(self, padding=(10, 4))
        bottom.pack(fill="x")
        ttk.Button(bottom, text=t("btn_refresh"),
                   command=self.refresh).pack(side="right")

    # ── Tab 内容构建 ──────────────────────────────────────────────────────────
    # 每个参数占两行：第 2n 行是控件，第 2n+1 行是说明文字
    def _build_tab(self, parent, controls):
        parent.columnconfigure(1, weight=1)

        for idx, ctrl in enumerate(controls):
            ctrl_row = idx * 2
            desc_row = idx * 2 + 1
            kind = ctrl[0]

            if kind == "toggle":
                name = ctrl[1]
                var = tk.IntVar(value=read_param(name))
                self._vars[name] = var
                kw = {"style": self._toggle_style} if self._toggle_style else {}
                ttk.Checkbutton(
                    parent, text=t(name), variable=var,
                    command=lambda n=name, v=var: write_param(n, v.get()),
                    **kw,
                ).grid(row=ctrl_row, column=0, columnspan=3,
                       sticky="w", pady=(6, 0))

            elif kind == "slider":
                _, name, lo, hi, res = ctrl
                var = tk.IntVar(value=read_param(name))
                self._vars[name] = var

                ttk.Label(parent, text=t(name), anchor="w",
                          ).grid(row=ctrl_row, column=0, sticky="w",
                                 padx=(0, 8), pady=(6, 0))

                def _on_slide(v, n=name, r=res, iv=var):
                    rounded = round(float(v) / r) * r
                    iv.set(rounded)
                    write_param(n, rounded)

                ttk.Scale(
                    parent, from_=lo, to=hi,
                    orient="horizontal", variable=var,
                    command=_on_slide,
                ).grid(row=ctrl_row, column=1, sticky="ew",
                       padx=2, pady=(6, 0))
                ttk.Label(parent, textvariable=var, width=7, anchor="e",
                          ).grid(row=ctrl_row, column=2, sticky="e",
                                 padx=(4, 0), pady=(6, 0))

            elif kind == "entry":
                name = ctrl[1]
                var = tk.StringVar(value=str(read_param(name)))
                self._vars[name] = var

                ttk.Label(parent, text=t(name), anchor="w",
                          ).grid(row=ctrl_row, column=0, sticky="w",
                                 padx=(0, 8), pady=(6, 0))
                ent = ttk.Entry(parent, textvariable=var, width=14)
                ent.grid(row=ctrl_row, column=1, sticky="w",
                         padx=2, pady=(6, 0))
                ent.bind("<Return>",   lambda e, n=name, v=var: self._commit(n, v))
                ent.bind("<FocusOut>", lambda e, n=name, v=var: self._commit(n, v))
                ttk.Label(parent, text=t("hint_entry"), foreground="#999",
                          font=self._font_small,
                          ).grid(row=ctrl_row, column=2, sticky="w",
                                 padx=4, pady=(6, 0))

            # 说明文字行（三列合并）
            d = desc(name)
            if d:
                ttk.Label(
                    parent, text=d, foreground="#888",
                    font=self._font_small, anchor="w",
                ).grid(row=desc_row, column=0, columnspan=3,
                       sticky="w", padx=(2, 0), pady=(0, 2))

    # ── 启动时检测当前模式 ────────────────────────────────────────────────────
    def _detect_mode(self) -> str:
        """读取设备当前值，若与游戏预设完全吻合则返回 'game'，否则返回 'daily'。"""
        return "game" if all(
            read_param(k) == v for k, v in GAME_PRESET.items()
        ) else "daily"

    # ── 模式切换 ──────────────────────────────────────────────────────────────
    def _on_mode(self):
        preset = GAME_PRESET if self._mode.get() == "game" else DAILY_PRESET
        for name, val in preset.items():
            write_param(name, val)
            var = self._vars.get(name)
            if var is not None:
                var.set(val if isinstance(var, tk.IntVar) else str(val))
        key = "hint_game" if self._mode.get() == "game" else "hint_daily"
        self._mode_hint.config(text=t(key))

    # ── Entry 提交 ────────────────────────────────────────────────────────────
    def _commit(self, name, var):
        try:
            write_param(name, int(var.get()))
        except ValueError:
            messagebox.showerror("无效值", t("err_invalid", val=var.get()))

    # ── 全参数刷新 ────────────────────────────────────────────────────────────
    def refresh(self):
        for name, var in self._vars.items():
            val = read_param(name)
            if isinstance(var, tk.StringVar):
                var.set(str(val))
            else:
                var.set(val)


def main():
    """Run the tuner UI."""
    App().mainloop()


if __name__ == "__main__":
    main()
