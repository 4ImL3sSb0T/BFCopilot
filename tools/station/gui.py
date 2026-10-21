"""Tkinter views; all network I/O runs on one worker."""

import queue
import math
import re
import threading
import tkinter as tk
from tkinter import filedialog, ttk
from tkinter.scrolledtext import ScrolledText

from station.link import available_ports

# 扑翼相关的参数：标定决定能不能动，限幅决定能动多大，两者都在参数表里，飞行固件读同一份。
CALIBRATION_PARAMS = (("l_min_us", "servo_l.angle_min_us"), ("l_max_us", "servo_l.angle_max_us"),
                      ("r_min_us", "servo_r.angle_min_us"), ("r_max_us", "servo_r.angle_max_us"),
                      ("l_calibrated", "servo_l.calibrated"), ("r_calibrated", "servo_r.calibrated"))
LIMIT_PARAMS = (("max_hz", "system.max_flapping_hz"), ("ramp_s", "system.ramp_s"),
                ("l_speed", "servo_l.max_speed_dps"), ("l_accel", "servo_l.max_acceleration_dps2"),
                ("r_speed", "servo_r.max_speed_dps"), ("r_accel", "servo_r.max_acceleration_dps2"))


class Station(tk.Tk):
    def __init__(self, link):
        super().__init__()
        self.link = link
        self.title("BFCopilot · 硬件调试站")
        self.geometry("1100x760")
        self.minsize(900, 620)
        self.requests = queue.Queue(maxsize=16)
        self.results = queue.Queue()
        self.closed = threading.Event()
        self.busy = False
        self.connected = False
        self.auto_discover = not link.port
        self.live_enabled = False
        self.status = tk.StringVar(value="未连接")
        self.port = tk.StringVar(value=link.port or "")
        self.live_command = tk.StringVar(value="hw adc")
        self.pulse_values = {side: tk.StringVar(value="1500") for side in ("l", "r", "both")}
        self.pending_pwm_stop = False
        self.console_command = tk.StringVar()
        self.flap_ceiling = tk.StringVar(value="填入限幅后估算可用幅度")
        self._build()
        self.protocol("WM_DELETE_WINDOW", self._close)
        threading.Thread(target=self._worker, daemon=True).start()
        self.after(50, self._drain)
        self.after(2000, self._poll)
        self.after(100, self._connect)

    def _build(self):
        style = ttk.Style(self)
        style.theme_use("clam")
        style.configure("TButton", padding=(10, 6))
        style.configure("TNotebook.Tab", padding=(16, 8))
        header = ttk.Frame(self, padding=12)
        header.pack(fill="x")
        ttk.Label(header, text="串口").pack(side="left")
        self.port_box = ttk.Combobox(header, textvariable=self.port, width=18,
                                     values=available_ports())
        self.port_box.pack(side="left", padx=6)
        ttk.Button(header, text="刷新端口", command=self._refresh_ports).pack(side="left")
        ttk.Button(header, text="连接", command=self._connect).pack(side="left", padx=6)
        ttk.Button(header, text="自动搜索", command=lambda: self._connect(search=True)).pack(side="left")
        ttk.Button(header, text="断开", command=lambda: self._submit("disconnect")).pack(side="left", padx=6)
        ttk.Button(header, text="停止全部 PWM", command=self._stop_pwm).pack(side="left", padx=6)
        ttk.Label(header, textvariable=self.status).pack(side="right")
        panes = ttk.Panedwindow(self, orient="vertical")
        panes.pack(fill="both", expand=True, padx=12, pady=(0, 12))
        notebook = ttk.Notebook(panes)
        panes.add(notebook, weight=3)
        live = self._tab(notebook, "实时读取")
        row = ttk.Frame(live)
        row.pack(fill="x")
        ttk.Combobox(row, textvariable=self.live_command, state="readonly", width=22,
                     values=("hw adc", "hw imu", "hw baro", "hw servo", "hw gpio 10", "flap")).pack(side="left")
        self.live_button = ttk.Button(row, text="开始刷新", command=self._toggle_live)
        self.live_button.pack(side="left", padx=8)
        ttk.Button(row, text="读取一次", command=lambda: self._run(self.live_command.get(), live=True)).pack(side="left")
        self.live_output = ScrolledText(live, height=10, font=("Consolas", 11), state="disabled")
        self.live_output.pack(fill="both", expand=True, pady=(10, 0))
        sensors = self._tab(notebook, "总线与传感器")
        self._buttons(sensors, (
            ("扫描器件", "hw scan"), ("IMU 原始寄存器", "hw imu"), ("气压计原始寄存器", "hw baro"),
            ("I2C 扫描", "hw i2c scan"), ("ADC 原始采样", "hw adc")))
        self._buttons(sensors, (
            ("IMU 连读 3 秒", "hw imu stream 3"), ("气压计连读 3 秒", "hw baro stream 3"),
            ("ADC 连读 3 秒", "hw adc stream 3"), ("复位 IMU", "hw imu reset")))
        ttk.Label(sensors, text="直接读取 SPI/I2C 寄存器和 ADC 原始计数、引脚毫伏值，不要求 sensors.calibrated。结果显示在下方控制台。",
                  wraplength=850).pack(anchor="w", pady=12)
        servo = self._tab(notebook, "舵机 · 直接 PWM")
        for side, label in (("l", "左舵机"), ("r", "右舵机"), ("both", "左右同时")):
            row = ttk.Labelframe(servo, text=label, padding=8)
            row.pack(fill="x", pady=5)
            ttk.Label(row, text="脉宽 µs").pack(side="left", padx=(0, 8))
            ttk.Spinbox(row, textvariable=self.pulse_values[side], from_=200, to=2800, increment=10, width=9).pack(side="left")
            for delta in (-50, -10, 10, 50):
                ttk.Button(row, text=f"{delta:+d} µs",
                           command=lambda selected=side, step=delta: self._send_pulse(selected, step)).pack(side="left", padx=4)
            ttk.Button(row, text="发送 PWM", command=lambda selected=side: self._send_pulse(selected)).pack(side="left", padx=4)
            ttk.Button(row, text="停止", command=lambda selected=side: self._run(f"hw pulse off {selected}")).pack(side="left", padx=4)
        self._buttons(servo, (("读取 PWM 状态", "hw servo"),))
        ttk.Label(servo, text="测量位置：左 H1 第3脚（GPIO18 / U1焊盘24），右 H2 第3脚（GPIO19 / U1焊盘25）；第1脚接地。",
                  wraplength=850).pack(anchor="w", pady=4)
        ttk.Label(servo, text="输入的是脉宽，不是角度。点击发送后直接驱动，无需标定；启动窗口不会发送 PWM。停止 PWM 不等于断电。",
                  wraplength=850).pack(anchor="w", pady=12)
        notebook.select(servo)
        flap = self._tab(notebook, "扑翼 · flapping service")
        self.calibration_values = {key: tk.StringVar(value="") for key in
                                   ("l_min_us", "l_max_us", "r_min_us", "r_max_us")}
        self.calibrated_flags = {"l": tk.BooleanVar(value=False), "r": tk.BooleanVar(value=False)}
        self.limit_values = {key: tk.StringVar(value="") for key in
                             ("max_hz", "ramp_s", "l_speed", "l_accel", "r_speed", "r_accel")}
        self.flap_values = {key: tk.StringVar(value=default) for key, default in (
            ("hz", "1"), ("amp_l", "8"), ("amp_r", "8"), ("center_l", "0"),
            ("center_r", "0"), ("phase", "0"), ("diff", "0"), ("seconds", "0"))}

        calibration = ttk.Labelframe(flap, padding=8,
                                     text="1. 两点标定（先用「舵机 · 直接 PWM」页量出不顶死的两个脉宽）")
        calibration.pack(fill="x", pady=5)
        for side, label in (("l", "左舵机"), ("r", "右舵机")):
            row = ttk.Frame(calibration)
            row.pack(fill="x", pady=3)
            ttk.Label(row, text=label).pack(side="left", padx=(0, 8))
            ttk.Label(row, text="下限 µs").pack(side="left", padx=(0, 2))
            ttk.Entry(row, textvariable=self.calibration_values[f"{side}_min_us"], width=8).pack(side="left", padx=(0, 10))
            ttk.Label(row, text="上限 µs").pack(side="left", padx=(0, 2))
            ttk.Entry(row, textvariable=self.calibration_values[f"{side}_max_us"], width=8).pack(side="left", padx=(0, 10))
            ttk.Checkbutton(row, text="已标定（calibrated=1）", variable=self.calibrated_flags[side]).pack(side="left")

        limits = ttk.Labelframe(flap, padding=8,
                                text="2. 限幅（默认值下加速度是瓶颈，不放开摆幅只有几度）")
        limits.pack(fill="x", pady=5)
        row = ttk.Frame(limits)
        row.pack(fill="x", pady=3)
        for key, label in (("max_hz", "最大频率 Hz"), ("ramp_s", "渐变 s"),
                           ("l_speed", "左速度 °/s"), ("l_accel", "左加速度 °/s²")):
            ttk.Label(row, text=label).pack(side="left", padx=(0, 2))
            ttk.Entry(row, textvariable=self.limit_values[key], width=9).pack(side="left", padx=(0, 12))
        row = ttk.Frame(limits)
        row.pack(fill="x", pady=3)
        for key, label in (("r_speed", "右速度 °/s"), ("r_accel", "右加速度 °/s²")):
            ttk.Label(row, text=label).pack(side="left", padx=(0, 2))
            ttk.Entry(row, textvariable=self.limit_values[key], width=9).pack(side="left", padx=(0, 12))

        config_actions = ttk.Frame(flap)
        config_actions.pack(fill="x", pady=4)
        ttk.Button(config_actions, text="写入标定与限幅", command=self._apply_config).pack(side="left", padx=(0, 6))
        ttk.Button(config_actions, text="读取当前值", command=self._read_config).pack(side="left")

        trajectory = ttk.Labelframe(flap, text="3. 轨迹（CONTROL 帧里的同一组数值）", padding=8)
        trajectory.pack(fill="x", pady=5)
        for row_fields in ((("hz", "频率 Hz"), ("amp_l", "左幅度 °"), ("amp_r", "右幅度 °"), ("diff", "差动")),
                           (("center_l", "左中心 °"), ("center_r", "右中心 °"), ("phase", "相位 °"),
                            ("seconds", "时长 s"))):
            row = ttk.Frame(trajectory)
            row.pack(fill="x", pady=3)
            for key, label in row_fields:
                ttk.Label(row, text=label).pack(side="left", padx=(0, 2))
                ttk.Entry(row, textvariable=self.flap_values[key], width=9).pack(side="left", padx=(0, 14))
        ceiling_row = ttk.Frame(flap)
        ceiling_row.pack(fill="x", pady=(2, 0))
        ttk.Label(ceiling_row, textvariable=self.flap_ceiling).pack(anchor="w")
        for variable in (self.flap_values["hz"], self.limit_values["l_speed"],
                         self.limit_values["l_accel"], self.limit_values["r_speed"],
                         self.limit_values["r_accel"]):
            variable.trace_add("write", self._update_flap_ceiling)
        actions = ttk.Frame(flap)
        actions.pack(fill="x", pady=4)
        ttk.Button(actions, text="开始扑翼", command=self._send_flap).pack(side="left", padx=(0, 6))
        ttk.Button(actions, text="停止扑翼", command=lambda: self._run("flap stop")).pack(side="left", padx=(0, 6))
        ttk.Button(actions, text="读取扑翼状态", command=lambda: self._run("flap")).pack(side="left")
        ttk.Label(flap, text="轨迹由固件的 flapping 服务生成（src/service/flapping），按 system.control_period_us 的周期"
                            "经舵机标定路径输出：台架上动的是产品自己的轨迹，不是上位机算出来的。\n"
                            "空着的输入框不会被写入，只发非空项；「读取当前值」把固件里的现值填回上面，改完再写。"
                            "取消标定请到控制台发 param set servo_<l|r>.calibrated 0。\n"
                            "写入前会先发 flap stop：输出开着的时候参数写入会被固件拒绝。写入标定按 "
                            "下限→上限→calibrated 的顺序，反了会被参数校验挡下。\n"
                            "幅度上限约 servo_*.max_acceleration_dps2 / (2π·f)²（默认 1 Hz 时约 9.1°）。"
                            "「停止扑翼」和顶栏「停止全部 PWM」都会停并关掉脉冲；停止 PWM 不等于断电。",
                  wraplength=1000, justify="left").pack(anchor="w", pady=12)
        io = self._tab(notebook, "LED / 蜂鸣器 / GPIO")
        self._form(io, "LED", (("R", "0", None), ("G", "0", None), ("B", "16", None)),
                   lambda values: "hw led " + " ".join(values))
        self._form(io, "蜂鸣器", (("Hz", "2000", None), ("ms", "200", None)),
                   lambda values: "hw buzzer " + " ".join(values))
        self._form(io, "GPIO", (("引脚", "10", None), ("操作", "read", ("read", "in", "out 0", "out 1"))),
                   lambda values: f"hw gpio {values[0]}" + ("" if values[1] == "read" else " " + values[1]))
        self._buttons(io, (("关闭 LED", "hw led off"), ("关闭蜂鸣器", "hw buzzer off")))
        registers = self._tab(notebook, "寄存器")
        self._form(registers, "器件寄存器", (
            ("器件", "imu", ("imu", "baro")), ("操作", "rd", ("rd", "wr")),
            ("寄存器", "0x0f", None), ("值", "0", None)),
            lambda values: f"hw {values[0]} {values[1]} {values[2]}" + (" " + values[3] if values[1] == "wr" else ""))
        self._form(registers, "I2C 寄存器", (
            ("操作", "rd", ("rd", "wr")), ("地址", "0x77", None),
            ("寄存器", "0x0d", None), ("值", "0", None)),
            lambda values: f"hw i2c {values[0]} {values[1]} {values[2]}" + (" " + values[3] if values[0] == "wr" else ""))
        overview = self._tab(notebook, "硬件总览")
        self._buttons(overview, (("SPI / I2C 扫描", "hw scan"), ("ADC 原始值", "hw adc"),
                                 ("PWM 状态", "hw servo"), ("底层命令帮助", "hw")))
        ttk.Label(overview, text="各页直接操作外设。器件读写失败会显示驱动错误码；不需要解锁、会话或参数标定。").pack(anchor="w", pady=12)
        console = ttk.Labelframe(panes, text="控制台", padding=8)
        panes.add(console, weight=2)
        command_row = ttk.Frame(console)
        command_row.pack(fill="x")
        entry = ttk.Entry(command_row, textvariable=self.console_command)
        entry.pack(side="left", fill="x", expand=True)
        entry.bind("<Return>", lambda event: self._send_console())
        ttk.Button(command_row, text="发送", command=self._send_console).pack(side="left", padx=6)
        ttk.Button(command_row, text="保存日志", command=self._save).pack(side="left")
        ttk.Button(command_row, text="清空", command=lambda: self._replace(self.log, "")).pack(side="left", padx=6)
        self.log = ScrolledText(console, height=12, font=("Consolas", 10), state="disabled")
        self.log.pack(fill="both", expand=True, pady=(8, 0))

    def _tab(self, notebook, title):
        frame = ttk.Frame(notebook, padding=14)
        notebook.add(frame, text=title)
        return frame

    def _buttons(self, parent, buttons):
        row = ttk.Frame(parent)
        row.pack(fill="x", pady=4)
        for label, command in buttons:
            ttk.Button(row, text=label, command=lambda line=command: self._run(line)).pack(side="left", padx=(0, 6))

    def _form(self, parent, title, fields, build):
        frame = ttk.Labelframe(parent, text=title, padding=8)
        frame.pack(fill="x", pady=5)
        variables = []
        for label, default, choices in fields:
            ttk.Label(frame, text=label).pack(side="left", padx=(0, 4))
            variable = tk.StringVar(value=default)
            variables.append(variable)
            if choices:
                widget = ttk.Combobox(frame, textvariable=variable, values=choices, state="readonly", width=8)
            else:
                widget = ttk.Entry(frame, textvariable=variable, width=26 if label == "参数名" else 10)
            widget.pack(side="left", padx=(0, 10))
        ttk.Button(frame, text="发送", command=lambda: self._run(build([variable.get().strip() for variable in variables]))).pack(side="left")

    def _connect(self, search=False):
        self.auto_discover = search or not self.port.get().strip()
        self.live_enabled = False
        self.live_button.configure(text="开始刷新")
        self._submit("connect", ("" if search else self.port.get(),))

    def _refresh_ports(self):
        self.port_box.configure(values=available_ports())

    def _submit(self, action, value=None, live=False):
        if self.busy:
            if not live:
                self.status.set("正在执行，请等待当前命令完成")
            return False
        self.busy = True
        self.status.set("正在连接…" if action == "connect" else "正在执行…")
        self.requests.put_nowait((action, value, live))
        return True

    def _run(self, line, live=False):
        if not self.connected:
            if not live:
                self.status.set("请先连接开发板")
            return
        self._submit("command", line, live)

    def _send_pulse(self, side, delta=0):
        if self.busy or not self.connected:
            self.status.set("正在执行，请等待当前命令完成" if self.busy else "请先连接开发板")
            return
        try:
            pulse = float(self.pulse_values[side].get()) + delta
            if not math.isfinite(pulse) or not 200 <= pulse <= 2800:
                raise ValueError
        except ValueError:
            self.status.set("脉宽必须是 200–2800 µs 范围内的数字")
            return
        self.pulse_values[side].set(f"{pulse:g}")
        self._run(f"hw pulse {side} {pulse:g}")

    def _send_flap(self):
        if self.busy or not self.connected:
            self.status.set("正在执行，请等待当前命令完成" if self.busy else "请先连接开发板")
            return
        arguments = []
        for key in ("hz", "amp_l", "amp_r", "center_l", "center_r", "phase", "diff"):
            try:
                number = float(self.flap_values[key].get())
                if not math.isfinite(number):
                    raise ValueError
            except ValueError:
                self.status.set(f"{key} 必须是有限数字")
                return
            arguments.append(f"{number:g}")
        seconds = self.flap_values["seconds"].get().strip()
        if seconds:
            try:
                duration = float(seconds)
                if not math.isfinite(duration) or duration < 0:
                    raise ValueError
            except ValueError:
                self.status.set("时长必须是不小于 0 的数字")
                return
            arguments.append(f"{duration:g}")
        self._run("flap " + " ".join(arguments))

    @staticmethod
    def _number_text(text, what):
        try:
            value = float(text)
        except ValueError:
            raise ValueError(f"{what} 必须是数字") from None
        if not math.isfinite(value):
            raise ValueError(f"{what} 必须是有限数字")
        return f"{value:g}"

    @staticmethod
    def _pulse_text(text, what):
        try:
            value = float(text)
        except ValueError:
            raise ValueError(f"{what} 必须是数字（µs）") from None
        if not math.isfinite(value) or value != int(value) or not 0 <= value <= 10000:
            raise ValueError(f"{what} 必须是 0–10000 之间的整数 µs")
        return str(int(value))

    def _config_variable(self, parameter):
        for key, name in CALIBRATION_PARAMS:
            if name == parameter:
                return self.calibration_values.get(key)
        for key, name in LIMIT_PARAMS:
            if name == parameter:
                return self.limit_values.get(key)
        return None

    def _apply_config(self):
        if self.busy or not self.connected:
            self.status.set("正在执行，请等待当前命令完成" if self.busy else "请先连接开发板")
            return
        # 输出打开时固件会拒绝改参数，所以先停扑翼。
        lines = ["flap stop"]
        try:
            for key, parameter in LIMIT_PARAMS:
                text = self.limit_values[key].get().strip()
                if text:
                    lines.append(f"param set {parameter} {self._number_text(text, parameter)}")
            for side, label in (("l", "左"), ("r", "右")):
                low = self.calibration_values[f"{side}_min_us"].get().strip()
                high = self.calibration_values[f"{side}_max_us"].get().strip()
                if low:
                    lines.append(f"param set servo_{side}.angle_min_us {self._pulse_text(low, label + '下限')}")
                if high:
                    lines.append(f"param set servo_{side}.angle_max_us {self._pulse_text(high, label + '上限')}")
                if self.calibrated_flags[side].get():
                    if not (low and high):
                        raise ValueError(f"{label}侧勾了「已标定」，但下限/上限 µs 空着；先填上或点「读取当前值」")
                    # calibrated 最后写：先写它的话，参数校验会拿还没写进去的脉宽去判，直接拒绝。
                    lines.append(f"param set servo_{side}.calibrated 1")
        except ValueError as error:
            self.status.set(str(error))
            return
        self._submit("sequence", lines)

    def _read_config(self):
        self._submit("refresh", [name for _, name in CALIBRATION_PARAMS + LIMIT_PARAMS])

    @staticmethod
    def _flap_envelope(frequency, speed, accel):
        """和 flapping_validate 用的两条包络同一套：peak·ω ≤ speed、peak·ω² ≤ accel。
        返回 (速度允许的幅度, 加速度允许的幅度)，单位度。"""
        omega = 2 * math.pi * frequency
        return speed / omega, accel / (omega * omega)

    def _update_flap_ceiling(self, *_):
        text = "填入最大频率、速度和加速度后估算可用幅度（或点「读取当前值」）"
        try:
            frequency = float(self.flap_values["hz"].get())
            if not math.isfinite(frequency) or frequency <= 0:
                raise ValueError
            parts = []
            for side, label in (("l", "左"), ("r", "右")):
                speed = float(self.limit_values[f"{side}_speed"].get())
                accel = float(self.limit_values[f"{side}_accel"].get())
                if not (math.isfinite(speed) and math.isfinite(accel)) or speed <= 0 or accel <= 0:
                    raise ValueError
                by_speed, by_accel = self._flap_envelope(frequency, speed, accel)
                parts.append(f"{label}侧 {min(by_speed, by_accel):.1f}°"
                             f"（速度 {by_speed:.1f} / 加速度 {by_accel:.1f}）")
            text = f"{frequency:g} Hz 下允许的峰值幅度：" + "；".join(parts) + "（未计入行程限制）"
        except ValueError:
            pass
        self.flap_ceiling.set(text)

    def _stop_pwm(self):
        if self.busy and self.connected:
            self.pending_pwm_stop = True
            self.live_enabled = False
            self.live_button.configure(text="开始刷新")
            self.status.set("当前命令结束后立即停止全部 PWM")
        else:
            self._run("hw pulse off")

    def _send_console(self):
        line = self.console_command.get().strip()
        if line and self.connected and not self.busy:
            self._run(line)
            self.console_command.set("")

    def _worker(self):
        while not self.closed.is_set():
            try:
                action, value, live = self.requests.get(timeout=0.2)
            except queue.Empty:
                continue
            try:
                if self.closed.is_set():
                    return
                if action == "connect":
                    self.link.set_target(*value)
                    output = self.link.connect()
                elif action == "disconnect":
                    self.link.close()
                    output = "已断开"
                elif action == "sequence":
                    transcript = []
                    for line in value:
                        transcript.append("> " + line)
                        transcript.append(self.link.run(line))
                    output = "\n".join(transcript)
                elif action == "refresh":
                    values = {}
                    for name in value:
                        match = re.match(r"\s*(\S+)\s*=\s*(\S+)", self.link.run(f"param get {name}"))
                        if match:
                            values[match.group(1)] = match.group(2)
                    output = values
                else:
                    output = self.link.run(value)
                self.results.put((action, value, live, output, None))
            except Exception as error:
                self.link.close()
                self.results.put((action, value, live, "", str(error)))
            finally:
                if self.closed.is_set():
                    self.link.close()

    def _drain(self):
        try:
            action, value, live, output, error = self.results.get_nowait()
        except queue.Empty:
            pass
        else:
            self.busy = False
            if error:
                self.connected = False
                self.live_enabled = False
                self.live_button.configure(text="开始刷新")
                self.status.set(error)
                self._append("错误: " + error + "\n")
            elif action == "connect":
                self.connected = True
                self.port.set(self.link.port)
                self._refresh_ports()
                self.status.set("已连接 " + output)
                self._append("已连接 " + output + "\n")
            elif action == "disconnect":
                self.connected = False
                self.auto_discover = False
                self.live_enabled = False
                self.live_button.configure(text="开始刷新")
                self.status.set(output)
            elif action == "sequence":
                self.status.set("有参数被拒绝，见控制台" if "set failed" in output else "参数已写入")
                self._append(output + "\n")
            elif action == "refresh":
                filled = 0
                for parameter, text in output.items():
                    variable = self._config_variable(parameter)
                    if variable is not None:
                        try:
                            variable.set(f"{float(text):g}")
                            filled += 1
                        except ValueError:
                            pass
                    if parameter in ("servo_l.calibrated", "servo_r.calibrated"):
                        side = "l" if parameter.startswith("servo_l.") else "r"
                        try:
                            self.calibrated_flags[side].set(float(text) != 0)
                        except ValueError:
                            pass
                self.status.set(f"已读取 {filled} 个参数值" if filled else "没有读到参数值，见控制台")
                self._append("> param get（当前值）\n"
                             + "\n".join(f"{name} = {value}" for name, value in sorted(output.items()))
                             + "\n")
            else:
                failed = any(marker in output for marker in (
                    "ESP_ERR_", "ESP_FAIL", "command failed:", "unknown command", "invalid command", "usage:"))
                self.status.set("硬件操作失败，见控制台" if failed else "已连接 " + str(self.link.port))
                if live:
                    self._replace(self.live_output, output)
                else:
                    self._append("> " + str(value) + "\n" + output + "\n")
            if self.pending_pwm_stop:
                self.pending_pwm_stop = False
                if self.connected:
                    self._run("hw pulse off")
        if not self.closed.is_set():
            self.after(50, self._drain)

    def _toggle_live(self):
        self.live_enabled = not self.live_enabled and self.connected
        self.live_button.configure(text="停止刷新" if self.live_enabled else "开始刷新")
        if self.live_enabled:
            self._run(self.live_command.get(), live=True)

    def _poll(self):
        if self.live_enabled and not self.busy:
            self._run(self.live_command.get(), live=True)
        elif not self.connected and self.auto_discover and not self.busy:
            self._submit("connect", ("",))
        if not self.closed.is_set():
            self.after(2000, self._poll)

    def _replace(self, widget, text):
        widget.configure(state="normal")
        widget.delete("1.0", "end")
        widget.insert("end", text)
        widget.configure(state="disabled")

    def _append(self, text):
        self.log.configure(state="normal")
        self.log.insert("end", text)
        if int(self.log.index("end-1c").split(".")[0]) > 5000:
            self.log.delete("1.0", "1001.0")
        self.log.see("end")
        self.log.configure(state="disabled")

    def _save(self):
        path = filedialog.asksaveasfilename(parent=self, defaultextension=".txt",
                                           filetypes=(("文本日志", "*.txt"),))
        if path:
            try:
                with open(path, "w", encoding="utf-8") as handle:
                    handle.write(self.log.get("1.0", "end-1c"))
            except OSError as error:
                self.status.set(str(error))

    def _close(self):
        self.closed.set()
        self.link.close()
        for callback in self.tk.splitlist(self.tk.call("after", "info")):
            self.after_cancel(callback)
        self.destroy()
