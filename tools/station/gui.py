"""Tkinter views; all network I/O runs on one worker."""

import queue
import math
import threading
import tkinter as tk
from tkinter import filedialog, ttk
from tkinter.scrolledtext import ScrolledText


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
        self.auto_discover = not link.host
        self.live_enabled = False
        self.status = tk.StringVar(value="未连接")
        self.host = tk.StringVar(value=link.host or "")
        self.port = tk.StringVar(value=str(link.port))
        self.live_command = tk.StringVar(value="hw adc")
        self.pulse_values = {side: tk.StringVar(value="1500") for side in ("l", "r", "both")}
        self.pending_pwm_stop = False
        self.console_command = tk.StringVar()
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
        ttk.Label(header, text="开发板 IP").pack(side="left")
        ttk.Entry(header, textvariable=self.host, width=22).pack(side="left", padx=6)
        ttk.Entry(header, textvariable=self.port, width=7).pack(side="left")
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
                     values=("hw adc", "hw imu", "hw baro", "hw servo", "hw gpio 10")).pack(side="left")
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
        self.auto_discover = search or not self.host.get().strip()
        self.live_enabled = False
        self.live_button.configure(text="开始刷新")
        self._submit("connect", ("" if search else self.host.get(), self.port.get()))

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
                self.host.set(self.link.host)
                self.status.set("已连接 " + output)
                self._append("已连接 " + output + "\n")
            elif action == "disconnect":
                self.connected = False
                self.auto_discover = False
                self.live_enabled = False
                self.live_button.configure(text="开始刷新")
                self.status.set(output)
            else:
                self.status.set("硬件操作失败，见控制台" if "ESP_ERR_" in output else "已连接 " + str(self.link.host))
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
            self._submit("connect", ("", self.port.get()))
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
