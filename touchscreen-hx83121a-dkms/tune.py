#!/usr/bin/env python3
"""Simple sysfs tuning GUI for the himax-spi touch algorithm."""

import glob
import tkinter as tk
from tkinter import messagebox
from pathlib import Path

# ── Locate algo sysfs directory ─────────────────────────────────────────────

def find_algo_dir():
    return Path('/sys/devices/platform/soc@0/9c0000.geniqup/998000.spi/spi_master/spi0/spi0.0/algo')
    matches = glob.glob("/sys/devices/**/algo/peak_threshold", recursive=True)
    if matches:
        return Path(matches[0]).parent
    return None

ALGO = find_algo_dir()

# ── Parameter definitions ────────────────────────────────────────────────────
#  (name, label, min, max, resolution)

SLIDERS = [
    # Detection thresholds
    ("macro_threshold",    "Macro threshold",      100, 2000, 10),
    ("peak_threshold",     "Peak threshold",        100, 2000, 10),
    # IIR
    ("iir_gate_floor",     "IIR gate floor",         20,  500,  5),
    ("iir_decay_weight",   "IIR decay weight",        0,  256,  1),
    ("iir_decay_step",     "IIR decay step",          0,  200,  1),
    # Palm rejection
    ("palm_area_threshold","Palm area threshold",     5,  150,  1),
    ("palm_density_low",   "Palm density low",       50,  800, 10),
    # CMF
    ("cmf_exclusion",      "CMF exclusion",          50,  800, 10),
    ("cmf_max_correction", "CMF max correction",     50, 1500, 10),
    # Edge compensation
    ("edge_boost_pct",     "Edge boost %",             0,  200,  5),
    ("edge_push_q8",       "Edge push (Q8.8)",         0,  384,  8),
    ("edge_blend_q8",      "Edge blend (Q8.8)",       64, 1024, 16),
    # Tracking
    ("track_lost_frames",  "Track lost frames",       1,   10,  1),
    ("debounce_base",      "Debounce base",           0,    8,  1),
    ("track_start_debounce","Track start debounce",    0,    8,  1),
]

TOGGLES = [
    ("cmf_enabled",        "CMF"),
    ("iir_enabled",        "IIR"),
    ("palm_enabled",       "Palm rejection"),
    ("pressure_enabled",   "Pressure reporting"),
    ("edge_comp_enabled",  "Edge compensation"),
    ("track_smoothing",    "Track smoothing"),
    ("track_active_guard", "Track active guard"),
]

# Parameters with large or irregular ranges — edited via Entry boxes
ENTRIES = [
    ("track_dist2_max",      "Track dist² max"),
    ("palm_signal_threshold","Palm signal threshold"),
    ("iir_noise_floor",      "IIR noise floor"),
    ("iir_gate_ratio_q8",    "IIR gate ratio (Q8)"),
]

# ── sysfs helpers ────────────────────────────────────────────────────────────

def read_param(name):
    try:
        return int((ALGO / name).read_text().strip())
    except Exception:
        return 0

def write_param(name, value):
    try:
        (ALGO / name).write_text(str(value))
    except PermissionError:
        messagebox.showerror("Permission denied",
                             f"Run as root to write {name}.")
    except Exception as e:
        messagebox.showerror("Error", str(e))

# ── GUI ──────────────────────────────────────────────────────────────────────

class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("Touch Algo Tuner")
        self.resizable(False, False)

        if ALGO is None:
            tk.Label(self, text="algo sysfs not found", fg="red",
                     padx=20, pady=20).pack()
            return

        tk.Label(self, text=str(ALGO), fg="gray", font=("mono", 9),
                 padx=8).pack(anchor="w")

        # ── Toggles ──
        toggle_frame = tk.LabelFrame(self, text="Switches", padx=8, pady=4)
        toggle_frame.pack(fill="x", padx=8, pady=(4, 0))
        self.bools = {}
        for col, (name, label) in enumerate(TOGGLES):
            var = tk.IntVar(value=read_param(name))
            self.bools[name] = var
            cb = tk.Checkbutton(toggle_frame, text=label, variable=var,
                                command=lambda n=name, v=var: write_param(n, v.get()))
            cb.grid(row=0, column=col, sticky="w", padx=8)

        # ── Sliders ──
        slider_frame = tk.LabelFrame(self, text="Parameters", padx=8, pady=4)
        slider_frame.pack(fill="x", padx=8, pady=4)
        slider_frame.columnconfigure(1, weight=1)

        self.vars = {}
        for row, (name, label, lo, hi, res) in enumerate(SLIDERS):
            current = read_param(name)
            var = tk.IntVar(value=current)
            self.vars[name] = var

            tk.Label(slider_frame, text=label, anchor="w",
                     width=22).grid(row=row, column=0, sticky="w")

            sl = tk.Scale(slider_frame, from_=lo, to=hi, resolution=res,
                          orient="horizontal", variable=var, length=320,
                          showvalue=False,
                          command=lambda v, n=name: write_param(n, int(v)))
            sl.grid(row=row, column=1, sticky="ew")

            val_lbl = tk.Label(slider_frame, textvariable=var, width=6, anchor="e")
            val_lbl.grid(row=row, column=2, sticky="e")

        # ── Entry boxes ──
        entry_frame = tk.LabelFrame(self, text="Large-range parameters",
                                    padx=8, pady=4)
        entry_frame.pack(fill="x", padx=8, pady=(0, 4))
        entry_frame.columnconfigure(1, weight=1)

        self.entries = {}
        for row, (name, label) in enumerate(ENTRIES):
            tk.Label(entry_frame, text=label, anchor="w",
                     width=22).grid(row=row, column=0, sticky="w")
            var = tk.StringVar(value=str(read_param(name)))
            self.entries[name] = var
            ent = tk.Entry(entry_frame, textvariable=var, width=14)
            ent.grid(row=row, column=1, sticky="w", padx=(4, 0))
            ent.bind("<Return>",   lambda e, n=name, v=var: self._write_entry(n, v))
            ent.bind("<FocusOut>", lambda e, n=name, v=var: self._write_entry(n, v))
            tk.Label(entry_frame, text="↵ to apply", fg="gray",
                     font=("", 8)).grid(row=row, column=2, sticky="w", padx=4)

        # ── Reset button ──
        tk.Button(self, text="Re-read from device",
                  command=self.refresh).pack(pady=(0, 6))

    def _write_entry(self, name, var):
        try:
            write_param(name, int(var.get()))
        except ValueError:
            messagebox.showerror("Invalid value",
                                 f"'{var.get()}' is not an integer.")

    def refresh(self):
        for name, var in self.vars.items():
            var.set(read_param(name))
        for name, var in self.bools.items():
            var.set(read_param(name))
        for name, var in self.entries.items():
            var.set(str(read_param(name)))


if __name__ == "__main__":
    App().mainloop()
