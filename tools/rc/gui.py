"""BFCopilot 专用遥控上位机（Tkinter）。

只连接飞控的 Wi-Fi/UDP 遥控入口，不复用台架 station。
"""

from __future__ import annotations

import json
import queue
import time
import tkinter as tk
from dataclasses import asdict, dataclass, field
from pathlib import Path
from tkinter import messagebox, ttk
from tkinter.scrolledtext import ScrolledText

from .joystick import AxisRoles
from .mapping import (
    MODE_FLAP, MODE_HOLD, MODE_LABELS, MODE_MANUAL, ManualControls, MappingConfig,
)
from .protocol import FlapCommand, FlapLimits, fault_labels
from .worker import Message, RcWorker, Telemetry

BG = "#0b1016"
PANEL = "#141b22"
PANEL2 = "#1b242e"
LINE = "#2a3744"
TEXT = "#e6edf3"
MUTED = "#8b98a5"
ACCENT = "#38bdf8"
OK = "#22c55e"
WARN = "#f59e0b"
DANGER = "#ef4444"
CONFIG_PATH = Path.home() / ".bfcopilot" / "rc.json"

MODE_COLORS = {MODE_HOLD: "#334155", MODE_MANUAL: "#3b4a5e", MODE_FLAP: "#0e7490"}


def _tuples(values, count):
    values = list(values) + [0.0] * count
    return tuple(float(value) for value in values[:count])


@dataclass
class AppConfig:
    host: str = ""
    port: int = 3335
    mapping: dict = field(default_factory=dict)
    manual: dict = field(default_factory=dict)
    limits: dict = field(default_factory=dict)
    roles: dict = field(default_factory=dict)


def load_config() -> AppConfig:
    try:
        data = json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return AppConfig()
    if not isinstance(data, dict):
        return AppConfig()
    config = AppConfig()
    config.host = str(data.get("host", config.host))
    try:
        config.port = int(data.get("port", config.port))
    except (TypeError, ValueError):
        pass
    for name in ("mapping", "manual", "limits", "roles"):
        value = data.get(name)
        if isinstance(value, dict):
            setattr(config, name, value)
    return config


def save_config(config: AppConfig) -> None:
    try:
        CONFIG_PATH.parent.mkdir(parents=True, exist_ok=True)
        CONFIG_PATH.write_text(json.dumps(asdict(config), ensure_ascii=False, indent=2),
                               encoding="utf-8")
    except OSError:
        pass


def mapping_from_dict(data: dict) -> MappingConfig:
    defaults = asdict(MappingConfig())
    defaults.update({key: value for key, value in data.items() if key in defaults})
    return MappingConfig(**defaults)


def manual_from_dict(data: dict) -> ManualControls:
    defaults = asdict(ManualControls())
    defaults.update({key: value for key, value in data.items() if key in defaults})
    return ManualControls(**defaults)


def limits_from_dict(data: dict) -> FlapLimits:
    defaults = asdict(FlapLimits())
    defaults.update({key: value for key, value in data.items() if key in defaults})
    for key in ("min_deg", "max_deg", "speed_dps", "acceleration_dps2"):
        defaults[key] = _tuples(defaults[key], 2)
    return FlapLimits(**defaults)


def roles_from_dict(data: dict) -> AxisRoles:
    defaults = asdict(AxisRoles())
    defaults.update({key: value for key, value in data.items() if key in defaults})
    for key in ("arm_button", "estop_button"):
        value = defaults.get(key)
        defaults[key] = None if value in (None, "", "无") else int(value)
    return AxisRoles(**defaults)


class RcStation(tk.Tk):
    def __init__(self, worker: RcWorker | None = None, config: AppConfig | None = None) -> None:
        super().__init__()
        self.worker = worker or RcWorker()
        self.config = config or load_config()
        self.mapping = mapping_from_dict(self.config.mapping)
        self.manual = manual_from_dict(self.config.manual)
        self.limits = limits_from_dict(self.config.limits)
        self.roles = roles_from_dict(self.config.roles)
        self.worker.mapping = self.mapping
        self.worker.manual = self.manual
        self.worker.limits = self.limits
        self.worker.roles = self.roles
        self.worker.joystick.configure(self.roles)
        self.worker.host = self.config.host
        self.worker.port = self.config.port

        self.telemetry: Telemetry | None = None
        self.closed = False
        self._arm_after = None
        self._arm_start = 0.0
        self._last_status_text = ""
        self._setting_vars: dict[str, tk.Variable] = {}
        self._role_vars: dict[str, tk.Variable] = {}

        self.title("BFCopilot · 遥控台")
        self.geometry("1320x860")
        self.minsize(1120, 760)
        self.configure(bg=BG)
        self._style()
        self._build()
        self._apply_config_to_widgets()
        self.protocol("WM_DELETE_WINDOW", self._close)
        self.bind_all("<Escape>", lambda _event: self._estop())
        self.worker.start()
        self._drain_after = self.after(50, self._drain)

    # -- styling ---------------------------------------------------------

    def _style(self) -> None:
        style = ttk.Style(self)
        style.theme_use("clam")
        style.configure(".", background=BG, foreground=TEXT, fieldbackground=PANEL2,
                        bordercolor=LINE, lightcolor=LINE, darkcolor=LINE)
        style.configure("TFrame", background=BG)
        style.configure("Panel.TFrame", background=PANEL)
        style.configure("Panel.TLabel", background=PANEL, foreground=TEXT)
        style.configure("Muted.TLabel", background=PANEL, foreground=MUTED)
        style.configure("Title.TLabel", background=BG, foreground=TEXT,
                        font=("Segoe UI Semibold", 16))
        style.configure("Value.TLabel", background=PANEL, foreground=ACCENT,
                        font=("Consolas", 12, "bold"))
        style.configure("TButton", padding=(10, 6), background=PANEL2, foreground=TEXT)
        style.map("TButton", background=[("active", "#243040")])
        style.configure("Accent.TButton", background="#0e7490", foreground="white")
        style.map("Accent.TButton", background=[("active", "#0891b2")])
        style.configure("Danger.TButton", background="#991b1b", foreground="white")
        style.map("Danger.TButton", background=[("active", "#b91c1c")])
        style.configure("TNotebook", background=BG, borderwidth=0)
        style.configure("TNotebook.Tab", padding=(16, 8), background=PANEL2,
                        foreground=MUTED)
        style.map("TNotebook.Tab", background=[("selected", PANEL)],
                  foreground=[("selected", TEXT)])
        style.configure("TEntry", fieldbackground=PANEL2, foreground=TEXT,
                        insertcolor=TEXT)
        style.configure("TCombobox", fieldbackground=PANEL2, foreground=TEXT)
        style.configure("TCheckbutton", background=PANEL, foreground=TEXT)
        style.configure("TRadiobutton", background=PANEL, foreground=TEXT)
        style.configure("TScale", background=PANEL)

    # -- layout ----------------------------------------------------------

    def _panel(self, parent, title: str) -> tk.Frame:
        frame = tk.Frame(parent, bg=PANEL, highlightbackground=LINE, highlightthickness=1)
        tk.Label(frame, text=title, bg=PANEL, fg=ACCENT,
                 font=("Segoe UI Semibold", 11), anchor="w").pack(fill="x", padx=12, pady=(10, 4))
        return frame

    def _build(self) -> None:
        header = tk.Frame(self, bg=BG)
        header.pack(fill="x", padx=16, pady=(12, 6))
        tk.Label(header, text="BFCopilot 遥控台", bg=BG, fg=TEXT,
                 font=("Segoe UI Semibold", 18)).pack(side="left")
        tk.Label(header, text="PC ↔ Wi-Fi/UDP ↔ 飞控 · BetaFPV 小白控三代", bg=BG,
                 fg=MUTED, font=("Segoe UI", 10)).pack(side="left", padx=(14, 0), pady=(6, 0))
        self.header_link = tk.Label(header, text="● 未连接", bg=BG, fg=MUTED,
                                    font=("Segoe UI Semibold", 11))
        self.header_link.pack(side="right")
        self.header_stick = tk.Label(header, text="摇杆：搜索中", bg=BG, fg=MUTED,
                                     font=("Segoe UI", 10))
        self.header_stick.pack(side="right", padx=(0, 18))

        body = tk.Frame(self, bg=BG)
        body.pack(fill="both", expand=True, padx=16, pady=6)
        body.columnconfigure(0, weight=0, minsize=300)
        body.columnconfigure(1, weight=1)
        body.columnconfigure(2, weight=0, minsize=280)
        body.rowconfigure(0, weight=1)

        left = tk.Frame(body, bg=BG)
        left.grid(row=0, column=0, sticky="nsew", padx=(0, 10))
        self._build_link_panel(left)
        self._build_flight_panel(left)
        self._build_power_panel(left)

        center = tk.Frame(body, bg=BG)
        center.grid(row=0, column=1, sticky="nsew", padx=10)
        self._build_mode_panel(center)
        self._build_sticks_panel(center)
        self._build_command_panel(center)

        right = tk.Frame(body, bg=BG)
        right.grid(row=0, column=2, sticky="nsew", padx=(10, 0))
        self._build_action_panel(right)
        self._build_log_panel(right)

        self.notebook = ttk.Notebook(self)
        self.notebook.pack(fill="both", expand=False, padx=16, pady=(6, 12))
        self._build_joystick_tab()
        self._build_link_tab()
        self._build_settings_tab()
        self._build_safety_tab()

    def _build_link_panel(self, parent) -> None:
        panel = self._panel(parent, "链路")
        panel.pack(fill="x", pady=(0, 10))
        row = tk.Frame(panel, bg=PANEL)
        row.pack(fill="x", padx=12, pady=4)
        tk.Label(row, text="飞控 IP", bg=PANEL, fg=MUTED).pack(side="left")
        self.host_var = tk.StringVar(value=self.config.host)
        ttk.Entry(row, textvariable=self.host_var, width=14).pack(side="left", padx=6)
        tk.Label(row, text="端口", bg=PANEL, fg=MUTED).pack(side="left")
        self.port_var = tk.StringVar(value=str(self.config.port))
        ttk.Entry(row, textvariable=self.port_var, width=6).pack(side="left", padx=6)
        buttons = tk.Frame(panel, bg=PANEL)
        buttons.pack(fill="x", padx=12, pady=(2, 8))
        ttk.Button(buttons, text="连接", style="Accent.TButton",
                   command=self._connect).pack(side="left")
        ttk.Button(buttons, text="断开", command=self._disconnect).pack(side="left", padx=6)
        ttk.Button(buttons, text="扫描", command=self._scan).pack(side="left")
        self.link_state = tk.Label(panel, text="未连接", bg=PANEL, fg=MUTED,
                                   anchor="w", font=("Segoe UI Semibold", 10))
        self.link_state.pack(fill="x", padx=12, pady=(0, 8))

    def _build_flight_panel(self, parent) -> None:
        panel = self._panel(parent, "飞控状态")
        panel.pack(fill="x", pady=(0, 10))
        self.state_label = tk.Label(panel, text="—", bg=PANEL, fg=MUTED,
                                    font=("Segoe UI Semibold", 20), anchor="w")
        self.state_label.pack(fill="x", padx=12)
        self.state_detail = tk.Label(panel, text="等待 STATUS", bg=PANEL, fg=MUTED,
                                     anchor="w", wraplength=260, justify="left")
        self.state_detail.pack(fill="x", padx=12, pady=(2, 4))
        self.faults_label = tk.Label(panel, text="故障：—", bg=PANEL, fg=MUTED,
                                     anchor="w", wraplength=260, justify="left")
        self.faults_label.pack(fill="x", padx=12, pady=(0, 10))

    def _build_power_panel(self, parent) -> None:
        panel = self._panel(parent, "电源 / 输出")
        panel.pack(fill="x")
        grid = tk.Frame(panel, bg=PANEL)
        grid.pack(fill="x", padx=12, pady=(0, 10))
        grid.columnconfigure(1, weight=1)
        self.power_vars = {}
        for row, (key, label) in enumerate((
                ("vbat", "电池电压"), ("iservo", "舵机电流"),
                ("left", "左舵机指令"), ("right", "右舵机指令"),
                ("phase", "扑翼相位"), ("rx_age", "控制帧年龄"),
                ("lease", "lease 剩余"), ("rejected", "拒绝帧"))):
            tk.Label(grid, text=label, bg=PANEL, fg=MUTED, anchor="w").grid(
                row=row, column=0, sticky="w", pady=1)
            variable = tk.StringVar(value="—")
            self.power_vars[key] = variable
            tk.Label(grid, textvariable=variable, bg=PANEL, fg=TEXT,
                     anchor="e", font=("Consolas", 10)).grid(row=row, column=1, sticky="e", pady=1)

    def _build_mode_panel(self, parent) -> None:
        panel = self._panel(parent, "模式")
        panel.pack(fill="x", pady=(0, 10))
        row = tk.Frame(panel, bg=PANEL)
        row.pack(fill="x", padx=12, pady=(0, 10))
        self.mode_buttons = {}
        for mode in (MODE_HOLD, MODE_MANUAL, MODE_FLAP):
            button = tk.Button(row, text=MODE_LABELS[mode], bg=MODE_COLORS[mode], fg=TEXT,
                               activebackground=ACCENT, activeforeground="#081018",
                               relief="flat", padx=18, pady=8, bd=0, cursor="hand2",
                               command=lambda value=mode: self._select_mode(value))
            button.pack(side="left", padx=(0, 8))
            self.mode_buttons[mode] = button
        tk.Label(panel, text="未解锁时只发保持中位；扑翼模式只在解锁后生效。", bg=PANEL,
                 fg=MUTED, anchor="w").pack(fill="x", padx=12, pady=(0, 8))

    def _build_sticks_panel(self, parent) -> None:
        panel = self._panel(parent, "摇杆")
        panel.pack(fill="x", pady=(0, 10))
        row = tk.Frame(panel, bg=PANEL)
        row.pack(fill="x", padx=12, pady=(0, 10))
        self.stick_left = tk.Canvas(row, width=190, height=190, bg="#0d141a",
                                    highlightthickness=1, highlightbackground=LINE)
        self.stick_left.pack(side="left")
        self.stick_right = tk.Canvas(row, width=190, height=190, bg="#0d141a",
                                     highlightthickness=1, highlightbackground=LINE)
        self.stick_right.pack(side="left", padx=14)
        info = tk.Frame(row, bg=PANEL)
        info.pack(side="left", fill="both", expand=True)
        self.stick_vars = {}
        for key, label in (("throttle", "油门"), ("yaw", "偏航"),
                          ("pitch", "俯仰"), ("roll", "横滚")):
            tk.Label(info, text=label, bg=PANEL, fg=MUTED, anchor="w").pack(fill="x")
            variable = tk.StringVar(value="0.00")
            self.stick_vars[key] = variable
            tk.Label(info, textvariable=variable, bg=PANEL, fg=TEXT, anchor="w",
                     font=("Consolas", 11)).pack(fill="x", pady=(0, 4))
        self.use_joystick_var = tk.BooleanVar(value=self.mapping.use_joystick)
        ttk.Checkbutton(panel, text="使用摇杆输入（关闭后用下方滑块）",
                        variable=self.use_joystick_var,
                        command=self._toggle_joystick).pack(anchor="w", padx=12, pady=(0, 8))

    def _build_command_panel(self, parent) -> None:
        panel = self._panel(parent, "命令")
        panel.pack(fill="both", expand=True)
        self.command_label = tk.Label(panel, text="—", bg=PANEL, fg=TEXT, anchor="w",
                                      justify="left", wraplength=560,
                                      font=("Consolas", 10))
        self.command_label.pack(fill="x", padx=12, pady=(0, 4))
        self.command_state = tk.Label(panel, text="等待链路", bg=PANEL, fg=MUTED, anchor="w",
                                      justify="left", wraplength=560)
        self.command_state.pack(fill="x", padx=12, pady=(0, 8))
        self.sliders = {}
        self.slider_labels = {}
        grid = tk.Frame(panel, bg=PANEL)
        grid.pack(fill="x", padx=12, pady=(0, 10))
        grid.columnconfigure(1, weight=1)
        specs = (
            ("frequency", "频率 Hz", 0.0, max(self.limits.max_frequency_hz, 0.1)),
            ("amplitude_left", "左幅度 °", 0.0, 90.0),
            ("amplitude_right", "右幅度 °", 0.0, 90.0),
            ("center_left", "左中心 °", -90.0, 90.0),
            ("center_right", "右中心 °", -90.0, 90.0),
            ("phase", "相位 °", -180.0, 180.0),
            ("differential", "差动", -1.0, 1.0),
        )
        for row, (key, label, low, high) in enumerate(specs):
            tk.Label(grid, text=label, bg=PANEL, fg=MUTED, anchor="w", width=10).grid(
                row=row, column=0, sticky="w")
            variable = tk.DoubleVar()
            self.sliders[key] = variable
            scale = ttk.Scale(grid, from_=low, to=high, variable=variable,
                              command=lambda _value, name=key: self._slider_changed(name))
            scale.grid(row=row, column=1, sticky="ew", padx=(0, 8), pady=2)
            value_label = tk.Label(grid, text="0", bg=PANEL, fg=TEXT, width=8,
                                   anchor="e", font=("Consolas", 10))
            value_label.grid(row=row, column=2, sticky="e")
            self.slider_labels[key] = value_label
            self.sliders[key + "_widget"] = scale

    def _build_action_panel(self, parent) -> None:
        panel = self._panel(parent, "操作")
        panel.pack(fill="x", pady=(0, 10))
        self.arm_button = tk.Button(panel, text="按住解锁", bg="#166534", fg="white",
                                    activebackground=OK, activeforeground="#06210f",
                                    relief="flat", bd=0, pady=12, cursor="hand2",
                                    font=("Segoe UI Semibold", 12))
        self.arm_button.pack(fill="x", padx=12, pady=(0, 8))
        self.arm_button.bind("<ButtonPress-1>", self._arm_press)
        self.arm_button.bind("<ButtonRelease-1>", self._arm_release)
        self.arm_button.bind("<Leave>", self._arm_release)
        self.blockers_label = tk.Label(panel, text="未连接", bg=PANEL, fg=MUTED, anchor="w",
                                       wraplength=250, justify="left")
        self.blockers_label.pack(fill="x", padx=12, pady=(0, 8))
        row = tk.Frame(panel, bg=PANEL)
        row.pack(fill="x", padx=12, pady=(0, 8))
        ttk.Button(row, text="上锁", command=self._disarm).pack(side="left")
        ttk.Button(row, text="清故障", command=self._clear).pack(side="left", padx=8)
        self.estop_button = tk.Button(panel, text="急停  ESC", bg="#7f1d1d", fg="white",
                                      activebackground=DANGER, activeforeground="white",
                                      relief="flat", bd=0, pady=14, cursor="hand2",
                                      font=("Segoe UI Semibold", 14),
                                      command=self._estop)
        self.estop_button.pack(fill="x", padx=12, pady=(0, 12))

    def _build_log_panel(self, parent) -> None:
        panel = self._panel(parent, "事件")
        panel.pack(fill="both", expand=True)
        self.log = ScrolledText(panel, height=16, bg="#0d141a", fg=TEXT,
                                insertbackground=TEXT, relief="flat", bd=0,
                                font=("Consolas", 9), wrap="word")
        self.log.pack(fill="both", expand=True, padx=10, pady=(0, 10))
        self.log.configure(state="disabled")
        self.log.tag_configure("ok", foreground=OK)
        self.log.tag_configure("warn", foreground=WARN)
        self.log.tag_configure("error", foreground=DANGER)

    # -- bottom tabs -----------------------------------------------------

    def _tab(self, title: str) -> tk.Frame:
        frame = tk.Frame(self.notebook, bg=PANEL)
        self.notebook.add(frame, text=title)
        return frame

    def _build_joystick_tab(self) -> None:
        frame = self._tab("摇杆映射")
        frame.columnconfigure(1, weight=0)
        frame.columnconfigure(2, weight=1)
        left = tk.Frame(frame, bg=PANEL)
        left.grid(row=0, column=0, sticky="nw", padx=12, pady=10)
        self.device_label = tk.Label(left, text="设备：搜索中", bg=PANEL, fg=MUTED,
                                     anchor="w", justify="left", wraplength=360)
        self.device_label.grid(row=0, column=0, columnspan=2, sticky="w")
        ttk.Button(left, text="刷新摇杆", command=self._refresh_joystick).grid(
            row=1, column=0, sticky="w", pady=6)
        ttk.Button(left, text="应用映射", command=self._apply_roles).grid(
            row=1, column=1, sticky="w", padx=8, pady=6)

        self._role_vars["throttle"] = tk.StringVar()
        self._role_vars["yaw"] = tk.StringVar()
        self._role_vars["pitch"] = tk.StringVar()
        self._role_vars["roll"] = tk.StringVar()
        self._role_vars["invert_throttle"] = tk.BooleanVar()
        self._role_vars["invert_yaw"] = tk.BooleanVar()
        self._role_vars["invert_pitch"] = tk.BooleanVar()
        self._role_vars["invert_roll"] = tk.BooleanVar()
        self._role_vars["arm_button"] = tk.StringVar()
        self._role_vars["estop_button"] = tk.StringVar()
        for row, (key, label) in enumerate((("throttle", "油门"), ("yaw", "偏航"),
                                            ("pitch", "俯仰"), ("roll", "横滚"))):
            tk.Label(left, text=label, bg=PANEL, fg=TEXT).grid(row=2 + row, column=0,
                                                               sticky="w", pady=2)
            ttk.Combobox(left, textvariable=self._role_vars[key], width=6, state="readonly",
                         values=[str(index) for index in range(8)]).grid(
                row=2 + row, column=1, sticky="w", padx=8)
            ttk.Checkbutton(left, text="反向", variable=self._role_vars["invert_" + key]).grid(
                row=2 + row, column=2, sticky="w")
        tk.Label(left, text="油门校准", bg=PANEL, fg=ACCENT).grid(row=6, column=0,
                                                                 sticky="w", pady=(10, 2))
        self.throttle_cal = tk.Label(left, text="低位 — / 高位 —", bg=PANEL, fg=MUTED)
        self.throttle_cal.grid(row=6, column=1, columnspan=2, sticky="w", padx=8)
        ttk.Button(left, text="记录最低点", command=lambda: self.worker.calibrate("low")).grid(
            row=7, column=0, sticky="w", pady=4)
        ttk.Button(left, text="记录最高点", command=lambda: self.worker.calibrate("high")).grid(
            row=7, column=1, sticky="w", padx=8, pady=4)
        for row, (key, label) in enumerate((("arm_button", "摇杆解锁键"),
                                            ("estop_button", "摇杆急停键")), start=8):
            tk.Label(left, text=label, bg=PANEL, fg=TEXT).grid(row=row, column=0, sticky="w")
            ttk.Combobox(left, textvariable=self._role_vars[key], width=6, state="readonly",
                         values=["无"] + [str(index) for index in range(16)]).grid(
                row=row, column=1, sticky="w", padx=8)

        self.axis_text = ScrolledText(frame, height=12, width=60, bg="#0d141a", fg=TEXT,
                                      insertbackground=TEXT, relief="flat", bd=0,
                                      font=("Consolas", 10))
        self.axis_text.grid(row=0, column=1, sticky="nsew", padx=(0, 12), pady=10)
        self.axis_text.configure(state="disabled")
        tk.Label(frame, text="提示：先看实时轴值确认哪个轴是油门，再点“应用映射”。"
                             "油门低位/高位用于把小白控的机械行程映射到 0..1。",
                 bg=PANEL, fg=MUTED, anchor="w", justify="left").grid(
            row=1, column=0, columnspan=2, sticky="w", padx=12, pady=(0, 10))
        frame.columnconfigure(2, weight=1)

    def _build_link_tab(self) -> None:
        frame = self._tab("链路详情")
        self.link_text = ScrolledText(frame, height=12, bg="#0d141a", fg=TEXT,
                                      insertbackground=TEXT, relief="flat", bd=0,
                                      font=("Consolas", 10))
        self.link_text.pack(fill="both", expand=True, padx=12, pady=10)
        self.link_text.configure(state="disabled")

    def _build_settings_tab(self) -> None:
        frame = self._tab("设置")
        grid = tk.Frame(frame, bg=PANEL)
        grid.pack(fill="x", padx=12, pady=10)
        specs = (
            ("deadband", "摇杆死区", self.mapping.deadband),
            ("expo", "摇杆指数", self.mapping.expo),
            ("center_range", "俯仰中心范围 °", self.mapping.center_range_deg),
            ("phase_range", "偏航相位范围 °", self.mapping.phase_range_deg),
            ("differential_range", "横滚差动范围", self.mapping.differential_range),
            ("max_frequency_hz", "本地最大频率 Hz", self.limits.max_frequency_hz),
            ("speed_l", "左速度限幅 °/s", self.limits.speed_dps[0]),
            ("speed_r", "右速度限幅 °/s", self.limits.speed_dps[1]),
            ("accel_l", "左加速度限幅 °/s²", self.limits.acceleration_dps2[0]),
            ("accel_r", "右加速度限幅 °/s²", self.limits.acceleration_dps2[1]),
            ("min_l", "左角度下限 °", self.limits.min_deg[0]),
            ("max_l", "左角度上限 °", self.limits.max_deg[0]),
            ("min_r", "右角度下限 °", self.limits.min_deg[1]),
            ("max_r", "右角度上限 °", self.limits.max_deg[1]),
            ("ramp_s", "斜坡时间 s", self.limits.ramp_s),
        )
        for row, (key, label, value) in enumerate(specs):
            tk.Label(grid, text=label, bg=PANEL, fg=TEXT, anchor="w").grid(
                row=row, column=0, sticky="w", pady=2)
            variable = tk.StringVar(value=str(value))
            self._setting_vars[key] = variable
            ttk.Entry(grid, textvariable=variable, width=10).grid(
                row=row, column=1, sticky="w", padx=8, pady=2)
        self._setting_vars["require_joystick"] = tk.BooleanVar(
            value=self.mapping.require_joystick)
        ttk.Checkbutton(frame, text="解锁必须连接摇杆（推荐）",
                        variable=self._setting_vars["require_joystick"]).pack(
            anchor="w", padx=12, pady=(4, 8))
        buttons = tk.Frame(frame, bg=PANEL)
        buttons.pack(anchor="w", padx=12, pady=(0, 10))
        ttk.Button(buttons, text="应用设置", command=self._apply_settings).pack(side="left")
        ttk.Button(buttons, text="保存到本机", command=self._save).pack(side="left", padx=8)
        tk.Label(frame, text="本地限幅只用于避免发出飞控必然拒绝的命令；最终以飞控参数为准。"
                             "改飞控参数请用飞控自己的 console。",
                 bg=PANEL, fg=MUTED, anchor="w", justify="left").pack(
            fill="x", padx=12, pady=(0, 10))

    def _build_safety_tab(self) -> None:
        frame = self._tab("安全说明")
        text = (
            "这条链路只用于台架/室内调试，不是可飞行链路。\n\n"
            "· 飞控侧必须用 CONFIG_BF_WIFI_LINK_ENABLED=y 构建；默认关闭。\n"
            "· 电脑先和飞控连到同一个热点（默认 STA 模式），再点“连接”。\n"
            "· 解锁必须由人明确操作：界面“按住解锁”或配置过的摇杆键按住 1 秒。\n"
            "· 未解锁时只发保持中位；摇杆控制时，扑翼需解锁后推油门；关闭摇杆后用滑块。\n"
            "· 飞控 CONTROL TTL 为 100 ms，控制帧或 lease 到期后自行失联锁定。\n"
            "· 使用摇杆时，USB 断开立即急停；重新插入后须重新连接、清故障并显式解锁。\n"
            "· 急停 ESTOP 会在飞控本地锁存；急停后必须重新 HELLO、CLEAR、CONTROL、ARM。\n"
            "· 当前没有应用层认证，同一热点上的客户端可以伪造帧；请使用专用 AP 和\n"
            "  强 WPA2 密码，不要把这条链路用于真实飞行。\n"
            "· PWM 关断不等于舵机断电；本板没有舵机电源开关。\n"
        )
        tk.Label(frame, text=text, bg=PANEL, fg=TEXT, justify="left", anchor="nw",
                 font=("Segoe UI", 10)).pack(fill="both", expand=True, padx=14, pady=12)

    # -- config <-> widgets ----------------------------------------------

    def _apply_config_to_widgets(self) -> None:
        if self.manual.mode == MODE_FLAP:
            # 保存的配置不携带运行授权；启动时回到保持，避免自动进入扑翼。
            self.manual.mode = MODE_HOLD
        self.host_var.set(self.config.host)
        self.port_var.set(str(self.config.port))
        self.use_joystick_var.set(self.mapping.use_joystick)
        self._role_vars["throttle"].set(str(self.roles.throttle))
        self._role_vars["yaw"].set(str(self.roles.yaw))
        self._role_vars["pitch"].set(str(self.roles.pitch))
        self._role_vars["roll"].set(str(self.roles.roll))
        for key in ("throttle", "yaw", "pitch", "roll"):
            self._role_vars["invert_" + key].set(getattr(self.roles, "invert_" + key))
        self._role_vars["arm_button"].set(
            "无" if self.roles.arm_button is None else str(self.roles.arm_button))
        self._role_vars["estop_button"].set(
            "无" if self.roles.estop_button is None else str(self.roles.estop_button))
        self.sliders["frequency"].set(self.manual.frequency_hz)
        self.sliders["amplitude_left"].set(self.manual.amplitude_left_deg)
        self.sliders["amplitude_right"].set(self.manual.amplitude_right_deg)
        self.sliders["center_left"].set(self.manual.center_left_deg)
        self.sliders["center_right"].set(self.manual.center_right_deg)
        self.sliders["phase"].set(self.manual.phase_deg)
        self.sliders["differential"].set(self.manual.differential)
        # 幅度滑块同时驱动摇杆映射和手动命令，避免两个配置源不一致。
        self.mapping.amplitude_left_deg = self.manual.amplitude_left_deg
        self.mapping.amplitude_right_deg = self.manual.amplitude_right_deg
        for key in ("frequency", "amplitude_left", "amplitude_right", "center_left",
                    "center_right", "phase", "differential"):
            self._update_slider_label(key)
        self._select_mode(self.manual.mode, send=False)
        self._update_slider_states()

    def _current_config(self) -> AppConfig:
        return AppConfig(
            host=self.host_var.get().strip(),
            port=int(self.port_var.get() or 3335),
            mapping=asdict(self.mapping),
            manual=asdict(self.manual),
            limits=asdict(self.limits),
            roles=asdict(self.roles),
        )

    def _save(self) -> None:
        try:
            save_config(self._current_config())
        except (TypeError, ValueError) as error:
            self._append_log(f"保存失败：{error}", "error")
            return
        self._append_log("设置已保存", "ok")

    # -- actions ---------------------------------------------------------

    def _connect(self) -> None:
        try:
            port = int(self.port_var.get())
        except ValueError:
            self._append_log("端口必须是整数", "error")
            return
        host = self.host_var.get().strip()
        if not host:
            self._append_log("飞控 IP 留空，先广播搜索…", "warn")
        self._sync_settings_to_worker()
        self.worker.connect(host, port)

    def _disconnect(self) -> None:
        self.worker.disconnect()

    def _scan(self) -> None:
        try:
            port = int(self.port_var.get())
        except ValueError:
            port = 3335
        self._append_log("正在广播查找飞控…")
        self.worker.scan(port)

    def _select_mode(self, mode: int, send: bool = True) -> None:
        if mode == MODE_FLAP:
            status = self.telemetry.snapshot.status if self.telemetry else None
            if not (status and status.armed):
                self._append_log("未解锁时不能切到扑翼模式", "warn")
                return
        self.manual.mode = mode
        for value, button in self.mode_buttons.items():
            button.configure(bg=MODE_COLORS[value] if value == mode else PANEL2,
                             fg=TEXT if value == mode else MUTED)
        if send:
            self.worker.set_manual(self.manual)

    def _toggle_joystick(self) -> None:
        self.mapping.use_joystick = bool(self.use_joystick_var.get())
        self.worker.set_mapping(self.mapping)
        self._update_slider_states()

    def _slider_changed(self, key: str) -> None:
        self._update_slider_label(key)
        value = float(self.sliders[key].get())
        if key == "amplitude_left":
            self.manual.amplitude_left_deg = value
            self.mapping.amplitude_left_deg = value
        elif key == "amplitude_right":
            self.manual.amplitude_right_deg = value
            self.mapping.amplitude_right_deg = value
        elif key == "frequency":
            self.manual.frequency_hz = value
        elif key == "center_left":
            self.manual.center_left_deg = value
        elif key == "center_right":
            self.manual.center_right_deg = value
        elif key == "phase":
            self.manual.phase_deg = value
        elif key == "differential":
            self.manual.differential = value
        self.worker.set_manual(self.manual)
        self.worker.set_mapping(self.mapping)

    def _update_slider_label(self, key: str) -> None:
        try:
            value = float(self.sliders[key].get())
        except (ValueError, KeyError):
            return
        label = self.slider_labels.get(key)
        if label is not None:
            label.configure(text=f"{value:.2f}")

    def _update_slider_states(self) -> None:
        enabled = not self.mapping.use_joystick
        for key in ("frequency", "center_left", "center_right", "phase", "differential"):
            widget = self.sliders.get(key + "_widget")
            if widget is not None:
                widget.configure(state="normal" if enabled else "disabled")
        for key in ("amplitude_left", "amplitude_right"):
            widget = self.sliders.get(key + "_widget")
            if widget is not None:
                widget.configure(state="normal")

    def _arm_press(self, _event=None) -> None:
        if self.telemetry is None:
            return
        if self.telemetry.blockers:
            self._append_log("解锁被阻止：" + "；".join(self.telemetry.blockers), "warn")
            return
        self._arm_start = time.monotonic()
        self._arm_tick()

    def _arm_tick(self) -> None:
        if not self._arm_start:
            return
        elapsed = time.monotonic() - self._arm_start
        if elapsed >= 1.2:
            self._arm_start = 0.0
            self.arm_button.configure(text="按住解锁")
            self.worker.arm()
            return
        self.arm_button.configure(text=f"继续按住 {1.2 - elapsed:.1f}s")
        self._arm_after = self.after(50, self._arm_tick)

    def _arm_release(self, _event=None) -> None:
        self._arm_start = 0.0
        if self._arm_after is not None:
            try:
                self.after_cancel(self._arm_after)
            except Exception:
                pass
            self._arm_after = None
        self.arm_button.configure(text="按住解锁")

    def _disarm(self) -> None:
        self.worker.disarm()

    def _clear(self) -> None:
        if not messagebox.askyesno("清除故障", "确认飞控已解除故障条件，并发送 CLEAR？"):
            return
        self.worker.clear()

    def _estop(self) -> None:
        self.worker.estop()
        self._append_log("已触发急停", "error")

    def _refresh_joystick(self) -> None:
        self._append_log("等待摇杆重新枚举…")

    def _apply_roles(self) -> None:
        def index(key: str, default: int) -> int:
            try:
                return int(self._role_vars[key].get())
            except (ValueError, TypeError):
                return default

        self.roles = AxisRoles(
            throttle=index("throttle", 2), yaw=index("yaw", 0),
            pitch=index("pitch", 1), roll=index("roll", 3),
            invert_throttle=bool(self._role_vars["invert_throttle"].get()),
            invert_yaw=bool(self._role_vars["invert_yaw"].get()),
            invert_pitch=bool(self._role_vars["invert_pitch"].get()),
            invert_roll=bool(self._role_vars["invert_roll"].get()),
            throttle_low=self.roles.throttle_low, throttle_high=self.roles.throttle_high,
            arm_button=self._button_index(self._role_vars["arm_button"]),
            estop_button=self._button_index(self._role_vars["estop_button"]),
        )
        self.worker.set_roles(self.roles)
        self._append_log("摇杆映射已应用", "ok")

    @staticmethod
    def _button_index(variable: tk.Variable) -> int | None:
        value = str(variable.get())
        if value in ("", "无"):
            return None
        try:
            return int(value)
        except ValueError:
            return None

    def _apply_settings(self) -> None:
        try:
            self.mapping.deadband = float(self._setting_vars["deadband"].get())
            self.mapping.expo = float(self._setting_vars["expo"].get())
            self.mapping.center_range_deg = float(self._setting_vars["center_range"].get())
            self.mapping.phase_range_deg = float(self._setting_vars["phase_range"].get())
            self.mapping.differential_range = float(self._setting_vars["differential_range"].get())
            self.mapping.require_joystick = bool(self._setting_vars["require_joystick"].get())
            maximum = float(self._setting_vars["max_frequency_hz"].get())
            self.limits = FlapLimits(
                max_frequency_hz=maximum, ramp_s=float(self._setting_vars["ramp_s"].get()),
                min_deg=(float(self._setting_vars["min_l"].get()),
                         float(self._setting_vars["min_r"].get())),
                max_deg=(float(self._setting_vars["max_l"].get()),
                         float(self._setting_vars["max_r"].get())),
                speed_dps=(float(self._setting_vars["speed_l"].get()),
                           float(self._setting_vars["speed_r"].get())),
                acceleration_dps2=(float(self._setting_vars["accel_l"].get()),
                                   float(self._setting_vars["accel_r"].get())),
            )
        except (ValueError, KeyError) as error:
            self._append_log(f"设置值无效：{error}", "error")
            return
        self.mapping.max_frequency_hz = maximum
        frequency_scale = self.sliders.get("frequency_widget")
        if frequency_scale is not None:
            frequency_scale.configure(to=max(maximum, 0.1))
        self.worker.set_mapping(self.mapping)
        self.worker.set_limits(self.limits)
        self._append_log("设置已应用", "ok")

    def _sync_settings_to_worker(self) -> None:
        self.worker.set_mapping(self.mapping)
        self.worker.set_manual(self.manual)
        self.worker.set_limits(self.limits)
        self.worker.set_roles(self.roles)

    # -- telemetry -------------------------------------------------------

    def _drain(self) -> None:
        if self.closed:
            return
        try:
            if not self.winfo_exists():
                return
        except tk.TclError:
            return
        for _ in range(64):
            try:
                message = self.worker.messages.get_nowait()
            except queue.Empty:
                break
            if message.kind == "telemetry":
                self._apply_telemetry(message.data)
            elif message.kind == "log":
                text = message.text
                tag = "error" if ("失败" in text or "急停" in text or "拒绝" in text) else \
                      "warn" if ("阻止" in text or "超时" in text or "未" in text) else "ok"
                self._append_log(text, tag)
            elif message.kind == "discovered":
                self.host_var.set(message.text)
                self._append_log(f"发现飞控 {message.text}", "ok")
        self._drain_after = self.after(50, self._drain)

    def _apply_telemetry(self, telemetry: Telemetry) -> None:
        self.telemetry = telemetry
        snapshot = telemetry.snapshot
        status = snapshot.status
        state = status.state if status else 0
        connected = snapshot.state == "connected" and telemetry.active
        armed = connected and bool(status and status.armed)
        if connected:
            self.header_link.configure(text="● 已连接", fg=OK)
            self.link_state.configure(text=f"会话 {snapshot.session:08x} · 序号 {snapshot.sequence}",
                                      fg=OK)
        elif snapshot.state == "hello":
            self.header_link.configure(text="● 等待 STATUS", fg=WARN)
            self.link_state.configure(text="HELLO 已发送，等待飞控", fg=WARN)
        elif snapshot.state == "closed":
            self.header_link.configure(text="● 会话关闭", fg=DANGER)
            self.link_state.configure(text=snapshot.reason or "已关闭", fg=DANGER)
        else:
            self.header_link.configure(text="● 未连接", fg=MUTED)
            self.link_state.configure(text="未连接", fg=MUTED)

        self.header_stick.configure(
            text=("摇杆：" + telemetry.device.name) if telemetry.device.connected
            else "摇杆：未连接", fg=OK if telemetry.device.connected else MUTED)

        if status is not None:
            color = OK if state == 2 else WARN if state in (1, 3, 4) else DANGER
            self.state_label.configure(text=f"{status.state_label}\n{status.state_name}", fg=color)
            self.state_detail.configure(
                text=f"armed={int(armed)} · lease {snapshot.lease:08x} · 遥测 {snapshot.status_age * 1000:.0f} ms")
            labels = fault_labels(status.faults)
            self.faults_label.configure(
                text=("故障：无" if not labels else "故障：" + "、".join(labels)),
                fg=MUTED if not labels else DANGER)
            self._set_power("vbat", "—" if status.vbat_mv is None else f"{status.vbat_mv / 1000:.2f} V")
            self._set_power("iservo", "—" if status.iservo_ma is None else f"{status.iservo_ma} mA")
            self._set_power("left", f"{status.left_cd / 100:+.1f}°")
            self._set_power("right", f"{status.right_cd / 100:+.1f}°")
            self._set_power("phase", f"{status.phase_cd / 100:+.1f}°")
            self._set_power("rx_age", f"{status.rx_age_ms} ms")
            self._set_power("lease", f"{status.lease_ms} ms")
            self._set_power("rejected", str(status.rejected))
        else:
            self.state_label.configure(text="—", fg=MUTED)
            self.state_detail.configure(text="等待 STATUS")
            self.faults_label.configure(text="故障：—", fg=MUTED)
            for key in self.power_vars:
                self._set_power(key, "—")

        sticks = telemetry.sticks
        self.stick_vars["throttle"].set(f"{sticks.throttle:.2f}")
        self.stick_vars["yaw"].set(f"{sticks.yaw:+.2f}")
        self.stick_vars["pitch"].set(f"{sticks.pitch:+.2f}")
        self.stick_vars["roll"].set(f"{sticks.roll:+.2f}")
        self._draw_sticks(sticks)
        self._update_slider_states()
        if self.mapping.use_joystick and sticks.connected:
            self._show_mapped_command(telemetry.command)
        self._show_command(telemetry)
        self._show_blockers(telemetry.blockers, connected, armed)
        self._update_joystick_tab(telemetry)
        self._update_link_tab(telemetry)
        self._update_mode_buttons(telemetry.mode, armed)

    def _set_power(self, key: str, text: str) -> None:
        variable = self.power_vars.get(key)
        if variable is not None:
            variable.set(text)

    def _show_command(self, telemetry: Telemetry) -> None:
        command = telemetry.command
        text = (f"mode={command.mode}  f={command.frequency_hz:.3f} Hz  "
                f"amp=({command.amplitude_deg[0]:.1f}, {command.amplitude_deg[1]:.1f})°  "
                f"center=({command.center_deg[0]:+.1f}, {command.center_deg[1]:+.1f})°  "
                f"phase={command.phase_deg:+.1f}°  diff={command.differential:+.3f}")
        self.command_label.configure(text=text)
        if telemetry.issues:
            self.command_state.configure(text="本地收敛：" + "；".join(telemetry.issues), fg=WARN)
        elif telemetry.command_ok:
            self.command_state.configure(text="命令有效", fg=OK)
        else:
            self.command_state.configure(text="命令无效", fg=DANGER)

    def _show_mapped_command(self, command: FlapCommand) -> None:
        self.sliders["frequency"].set(command.frequency_hz)
        self.sliders["center_left"].set(command.center_deg[0])
        self.sliders["center_right"].set(command.center_deg[1])
        self.sliders["phase"].set(command.phase_deg)
        self.sliders["differential"].set(command.differential)
        for key in ("frequency", "center_left", "center_right", "phase", "differential"):
            self._update_slider_label(key)

    def _show_blockers(self, blockers: list[str], connected: bool, armed: bool) -> None:
        if armed:
            text, color = "已解锁：飞控正在执行命令", OK
        elif not connected:
            text, color = "未连接：先连接飞控", MUTED
        elif blockers:
            text, color = "解锁阻止：" + "；".join(blockers), WARN
        else:
            text, color = "可以解锁：按住按钮 1.2 秒", OK
        self.blockers_label.configure(text=text, fg=color)
        state = "normal" if connected and not blockers and not armed else "disabled"
        self.arm_button.configure(state=state)

    def _update_mode_buttons(self, mode: int, armed: bool) -> None:
        for value, button in self.mode_buttons.items():
            selected = value == mode
            button.configure(bg=MODE_COLORS[value] if selected else PANEL2,
                             fg=TEXT if selected else MUTED)
            if value == MODE_FLAP:
                button.configure(state="normal" if armed else "disabled")

    def _draw_sticks(self, sticks) -> None:
        self._draw_stick(self.stick_left, sticks.yaw, sticks.throttle,
                         "油门 / 偏航", throttle=True)
        self._draw_stick(self.stick_right, sticks.roll, sticks.pitch,
                         "俯仰 / 横滚", throttle=False)

    def _draw_stick(self, canvas: tk.Canvas, x: float, y: float,
                    title: str, throttle: bool) -> None:
        canvas.delete("all")
        width = int(canvas["width"])
        height = int(canvas["height"])
        canvas.create_rectangle(10, 10, width - 10, height - 30, outline=LINE)
        canvas.create_line(width / 2, 10, width / 2, height - 30, fill="#243040")
        canvas.create_line(10, (height - 20) / 2, width - 10, (height - 20) / 2, fill="#243040")
        if throttle:
            px = 10 + (max(-1.0, min(1.0, x)) + 1.0) * 0.5 * (width - 20)
            py = height - 30 - max(0.0, min(1.0, y)) * (height - 40)
        else:
            px = 10 + (max(-1.0, min(1.0, x)) + 1.0) * 0.5 * (width - 20)
            py = 10 + (1.0 - (max(-1.0, min(1.0, y)) + 1.0) * 0.5) * (height - 40)
        canvas.create_oval(px - 9, py - 9, px + 9, py + 9, outline=ACCENT, width=2)
        canvas.create_oval(px - 3, py - 3, px + 3, py + 3, fill=ACCENT, outline="")
        canvas.create_text(width / 2, height - 14, text=title, fill=MUTED,
                           font=("Segoe UI", 9))

    def _update_joystick_tab(self, telemetry: Telemetry) -> None:
        device = telemetry.device
        if device.connected:
            self.device_label.configure(
                text=f"设备：{device.name}\n轴 {device.axes} · 按键 {device.buttons} · "
                     f"hat {device.hats}", fg=OK)
        else:
            self.device_label.configure(
                text=f"设备：未连接\n{device.error or '插入小白控三代后自动重试'}", fg=WARN)
        sticks = telemetry.sticks
        lines = [f"{index:>2}: {value:+.3f}" for index, value in enumerate(sticks.axes)]
        if lines:
            lines.append("")
            pressed = [str(index) for index, value in enumerate(sticks.buttons) if value]
            lines.append("按键按下：" + (", ".join(pressed) if pressed else "无"))
        lines.append("")
        lines.append(f"油门校准：低位 {self.roles.throttle_low:+.3f} / "
                     f"高位 {self.roles.throttle_high:+.3f}")
        self.axis_text.configure(state="normal")
        self.axis_text.delete("1.0", "end")
        self.axis_text.insert("1.0", "\n".join(lines))
        self.axis_text.configure(state="disabled")
        self.throttle_cal.configure(
            text=f"低位 {self.roles.throttle_low:+.3f} / 高位 {self.roles.throttle_high:+.3f}")

    def _update_link_tab(self, telemetry: Telemetry) -> None:
        snapshot = telemetry.snapshot
        lines = [
            f"会话状态    {snapshot.state}  {snapshot.reason}",
            f"session     {snapshot.session:08x}",
            f"sequence    {snapshot.sequence}",
            f"status seq  {snapshot.status_sequence}",
            f"lease       {snapshot.lease:08x}",
            f"遥测年龄    {snapshot.status_age * 1000:.0f} ms",
            f"控制年龄    {snapshot.control_age * 1000:.0f} ms",
            f"UDP         tx {snapshot.tx} / rx {snapshot.rx} / bad {snapshot.bad}",
            f"最后收包    {telemetry.link_age * 1000:.0f} ms 前",
            f"拒绝帧      {snapshot.rejected}（本周期 +{snapshot.rejected_delta}）",
            f"待确认请求  {len(snapshot.pending)}",
        ]
        if self.worker.session.last_ack:
            kind, result, at = self.worker.session.last_ack
            lines.append(f"最后 ACK    {kind} {'成功' if result == 0 else '拒绝'} "
                         f"({time.strftime('%H:%M:%S', time.localtime(at))})")
        self.link_text.configure(state="normal")
        self.link_text.delete("1.0", "end")
        self.link_text.insert("1.0", "\n".join(lines))
        self.link_text.configure(state="disabled")

    def _append_log(self, text: str, tag: str = "ok") -> None:
        self.log.configure(state="normal")
        self.log.insert("end", text.rstrip() + "\n", tag)
        self.log.see("end")
        self.log.configure(state="disabled")
        self._last_status_text = text

    # -- close -----------------------------------------------------------

    def _close(self) -> None:
        if self.closed:
            return
        self.closed = True
        try:
            save_config(self._current_config())
        except Exception:
            pass
        try:
            self.worker.shutdown()
        except Exception:
            pass
        self.after(250, self._finish_close)

    def _finish_close(self) -> None:
        self.closed = True
        for attribute in ("_drain_after", "_arm_after"):
            pending = getattr(self, attribute, None)
            if pending is not None:
                try:
                    self.after_cancel(pending)
                except Exception:
                    pass
                setattr(self, attribute, None)
        try:
            self.worker.stop()
        except Exception:
            pass
        # Tk 变量在解释器销毁后由 GC 回收时会在错误线程里调用 Tcl；
        # 在窗口仍然有效、且位于主线程时先主动释放它们。
        for mapping in (getattr(self, "power_vars", {}), getattr(self, "stick_vars", {}),
                        getattr(self, "sliders", {}), getattr(self, "_setting_vars", {}),
                        getattr(self, "_role_vars", {})):
            mapping.clear()
        for name, value in list(self.__dict__.items()):
            if isinstance(value, tk.Variable):
                setattr(self, name, None)
        self.destroy()


def main() -> None:
    RcStation().mainloop()
