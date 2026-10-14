import sys
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))


class FakeHardwareLink:
    host = "fake-board"
    port = 3333

    def __init__(self):
        self.commands = []

    def set_target(self, host, port):
        self.host = host
        self.port = int(port)

    def connect(self):
        return "fake-board:3333"

    def run(self, command):
        self.commands.append(command)
        return "calibrated=0" if command == "hw servo" else "ESP_OK"

    def close(self):
        pass


class HardwareGuiTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            import tkinter
        except ImportError as error:
            raise unittest.SkipTest(f"Tkinter unavailable: {error}")
        try:
            probe = tkinter.Tk()
            probe.withdraw()
            probe.destroy()
        except tkinter.TclError as error:
            raise unittest.SkipTest(f"Tcl/Tk unavailable: {error}")

    def setUp(self):
        from station.gui import Station

        self.link = FakeHardwareLink()
        self.window = Station(self.link)
        self.window.withdraw()
        self.addCleanup(self.window._close)
        self.wait_for(lambda: self.window.connected)

    def wait_for(self, condition):
        deadline = time.monotonic() + 2
        while not condition() and time.monotonic() < deadline:
            self.window.update()
            time.sleep(0.005)
        self.assertTrue(condition())

    def widgets(self, parent):
        for child in parent.winfo_children():
            yield child
            yield from self.widgets(child)

    def test_uncalibrated_pwm_controls_send_raw_commands(self):
        self.assertEqual(self.link.commands, [])
        self.window._run("hw servo")
        self.wait_for(lambda: not self.window.busy)
        for side in ("l", "r", "both"):
            self.window.pulse_values[side].set("1530")
            self.window._send_pulse(side)
            self.wait_for(lambda: not self.window.busy)
            self.assertEqual(self.link.commands[-1], f"hw pulse {side} 1530")
        self.window._send_pulse("l", -10)
        self.wait_for(lambda: not self.window.busy)
        self.assertEqual(self.link.commands[-1], "hw pulse l 1520")
        self.window._stop_pwm()
        self.wait_for(lambda: not self.window.busy)
        self.assertEqual(self.link.commands[-1], "hw pulse off")

    def test_invalid_pulses_do_not_reach_hardware(self):
        for value in ("nan", "inf", "199", "2801", "oops"):
            self.window.pulse_values["l"].set(value)
            self.window._send_pulse("l")
        self.assertEqual(self.link.commands, [])

    def test_stop_runs_after_the_current_command(self):
        self.window._run("hw imu stream 3")
        self.window.live_enabled = True
        self.window._stop_pwm()
        self.assertFalse(self.window.live_enabled)
        self.wait_for(lambda: not self.window.busy)
        self.assertEqual(self.link.commands, ["hw imu stream 3", "hw pulse off"])

    def test_every_device_form_and_shortcut_uses_hw_commands(self):
        from tkinter import ttk

        buttons = [
            widget for widget in self.widgets(self.window)
            if isinstance(widget, ttk.Button) and (
                widget.cget("text") == "发送"
                or widget.cget("text") in (
                    "扫描器件", "IMU 原始寄存器", "气压计原始寄存器", "ADC 原始采样",
                    "I2C 扫描", "关闭 LED", "关闭蜂鸣器", "读取 PWM 状态"))
        ]
        for button in buttons:
            button.invoke()
            self.wait_for(lambda: not self.window.busy)
        expected = (
            "hw scan", "hw imu", "hw baro", "hw adc", "hw i2c scan",
            "hw led 0 0 16", "hw buzzer 2000 200", "hw gpio 10",
            "hw imu rd 0x0f", "hw i2c rd 0x77 0x0d",
            "hw led off", "hw buzzer off", "hw servo")
        for command in expected:
            self.assertIn(command, self.link.commands)
        self.assertTrue(all(command.startswith("hw ") for command in self.link.commands))


if __name__ == "__main__":
    unittest.main()
