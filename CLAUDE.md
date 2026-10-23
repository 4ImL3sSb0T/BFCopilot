# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

BFCopilot is flapping-wing (bionic butterfly) flight-controller firmware for the current
butterfly PCB: an ESP32-C6 flight controller plus a second ESP32 as the remote, linked by
encrypted ESP-NOW. Written in C on **ESP-IDF v6.0.2**, target `esp32c6` (see `sdkconfig.defaults`).

Two firmwares share one project tree, selected at configure time:
- **Flight controller** (default) — entry point `src/app/app_main.c`.
- **Remote** — entry point `src/app/remote_main.c`, built with `-D BF_REMOTE=1`. It is only a bridge:
  it does not implement control logic, it converts COBS-framed USB/UART frames to ESP-NOW and back.

A third image is the **hardware test firmware** in `test/bench/` — a separate ESP-IDF project
that links only the components that own hardware (board, parameter, servo, sensors) and serves a
console over Wi-Fi, with the USB-Serial-JTAG port as the always-available fallback, so the board
can be brought up part by part. It deliberately runs no flight logic: see [Dedicated hardware test
firmware](#dedicated-hardware-test-firmware-wi-fi-console-with-usb-serial-fallback).

`docs/PLAN.md` is the authoritative requirements document (phases, exit conditions, safety rules).
Read it before implementing anything. All docs are in Chinese; keep Chinese for user-facing
explanations and doc edits.

## Commands

### Firmware build / flash / monitor

`idf.py` must be launched through the wrapper — MSYS2 (git-bash) re-injects `MSYSTEM` into native
child processes, and `idf.py` silently skips its own `main()` when it sees `MSYSTEM`. The wrapper
clears it and sets the local ESP-IDF / Espressif tool paths. Those paths are absolute and
machine-specific; update `scripts/idf.cmd` if the toolchain moves.

```bash
scripts/idf.sh build          # git-bash
scripts/idf.sh flash monitor
scripts/idf.sh menuconfig
scripts/idf.sh -D BF_REMOTE=1 -B build-remote build     # remote firmware
scripts/idf.sh fullclean
```

On cmd.exe use `scripts\idf.cmd <args>` (same arguments pass through).

Wireless is **off by default** (`CONFIG_BF_RADIO_ENABLED=n`). To build a radio-enabled image use a
separate build directory with provisioning defaults, and replace the dummy peer/keys:

```bash
scripts/idf.sh -B build-radio -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;test/radio.defaults" build
```

`test/radio.defaults` also disables the console because the remote firmware owns UART0. `sdkconfig`
is gitignored — anything that must survive a clean checkout belongs in `sdkconfig.defaults`.

The current RC scheme is **PC Wi-Fi ↔ flight controller directly**. It is a separate build option,
mutually exclusive with ESP-NOW (`CONFIG_BF_WIFI_LINK_ENABLED`):

```bash
cp scripts/wifi.local.defaults.example wifi.local.defaults   # set a WPA2 passphrase
scripts/idf.sh -B build-wifi -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;wifi.local.defaults" build
scripts/idf.sh -B build-wifi -p COM4 flash monitor
```

The flight controller runs a WPA2 AP (default SSID `BFCopilot`, address `192.168.4.1`) and accepts
the same 60-byte frames over UDP (`CONFIG_BF_WIFI_LINK_PORT`, default 3335); the computer connects to
that AP. `wifi.local.defaults` is gitignored. This link has **no application-layer authentication** —
it is bench/indoor only, not a flight-worthy link.

### Dedicated hardware test firmware (Wi-Fi console with USB serial fallback)

`test/bench/` is its own ESP-IDF project for bringing a board up part by part. It links the
components that own hardware — `board`, `parameter`, `servo`, `sensors` — plus `flapping`, so the
real trajectory generator can be driven on real servos. The rest of the flight stack (`radio`,
`flight`, `safety`, `telemetry`, `bf_console`, `protocol`) is excluded: nothing a test does can be
gated on an arming or fault state, and no control loop or self-check is running to fight it. It
supplies its own link (`net`) and its own commands (`hwtest`, `flap`) under `test/bench/src/app/`.

Credentials are not committed. Copy `test/bench/sdkconfig.defaults` to `local.defaults` (gitignored),
fill in SSID/passphrase, then:

```bash
scripts/idf.sh -C test/bench -B "$PWD/test/bench/build" \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;local.defaults" -p COM4 flash monitor
```

`-B` must be absolute: `idf.py` resolves a relative `-B` against the original working directory,
before `-C` takes effect. The firmware logs its address once associated and serves the console on
`CONFIG_BF_BENCH_PORT` (3333).

The **USB-Serial-JTAG console is always available** on the programming port: `sdkconfig.defaults`
selects it as the primary console (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`), and `app_main` does not
abort when Wi-Fi is unconfigured or the AP is unreachable — it logs a warning and the board stays
reachable over the cable. Both transports run the same `esp_console` command set, so the serial
port is a full fallback, not just a log channel.

`tools/bf_station.py` is a native Python/Tkinter desktop UI over that console: a live pane, per-device test buttons, raw
pulse control for the servos, a flapping page (two-point calibration, limits, trajectory), LED/buzzer/GPIO, register
pokes and the console itself. Its default `auto` method tries Wi-Fi/TCP first (scanning for the port — DHCP moves the
address on every reset — and keeping looking when the board is away), then falls back to the USB-Serial-JTAG serial port
(Espressif VID first, Bluetooth scan ports skipped). The header also lets you pin `Wi-Fi/TCP` or `串口（USB-JTAG）`.
`--host`/`--serial` skip discovery, `--method` pins the transport, and `--list` prints the serial ports.

```bash
uv run --project tools python tools/bf_station.py
uv run --project tools python tools/bf_station.py --method serial --serial COM4
```

Commands (`hw` with no argument lists them): `hw scan` probes both buses and separates "no device
answered" from "the bus failed"; `hw imu [rd <reg> | wr <reg> <val> | reset | stream [s]]` and
`hw baro [rd <reg> [addr] | wr <reg> <val> [addr] | stream [s]]` are register-level; `hw i2c` is the
bus without naming a device; `hw adc` is raw counts plus converted millivolts; `hw pulse <l|r|both>
<us>` and `hw pulse off` drive a raw servo pulse with no calibration; `hw servo` reports the last
commanded pulse against the calibrated mapping; `hw led`, `hw buzzer` and `hw gpio <pin> [in | out
<0|1>]` cover the rest. `status`, `params`, `param get/set/reset` and `servo <l|r|both> <deg>` round
it out, and `help` lists everything.

One command sits outside `hw`: `flap <hz> <amp_l> <amp_r> <center_l> <center_r> <phase_deg> <diff>
[seconds]` runs the real flapping service on the servos — the parameter store's limits, the flight
controller's tick period, the calibrated servo path — and needs both sides calibrated before it will
emit anything. With no arguments it prints the trajectory state and the current limits; `flap stop`
ends it. `hw pulse off` and `servo off` double as its emergency stop. While flapping, `hw pulse <us>`
and `servo <deg>` are refused: they would fight the trajectory for the same compare value. It exists
to exercise `src/service/flapping` on real hardware, and no flight path calls it.

Bus access goes through `sensors_debug.h`, which uses the same pins, clock and mode as the flight
firmware but bypasses the drivers' identity and configuration checks on purpose, so it still answers
for a device the driver already rejected. No flight path may call that header. Servo raw pulse uses
`servo_debug_*` for the same reason.

**Not the flight firmware.** No safety state machine, no session, no arming; a commanded pulse holds
until you turn it off or reboot. Never flash it on a vehicle meant to fly, and do not leave it
driving servos unattended.

### Host unit tests (no hardware, no ESP-IDF)

Two suites built by CMake: `bf_host_tests` (pure logic: CRC, fault codes, angle→pulse mapping,
flapping trajectory, frame codec) and `bf_flight_tests` (flight/safety state machine against
stubbed ESP-IDF headers in `test/host/stubs/`).

```bash
cd test/host
cmake -B build && cmake --build build
ctest --test-dir build --output-on-failure
```

Run a single suite with `-R bf_flight_tests`, or run the test binary directly for raw output.

### Protocol / host-tool tests (Python)

`python` on PATH is the Windows Store stub and does nothing; use `uv` with the `tools` project.
`pyserial` is only imported by the live tool, so the protocol tests run without hardware.

```bash
uv run --project tools python -m unittest discover -s test -p "test_*.py"
uv run --project tools python tools/bf_ground.py --list         # enumerate serial ports
uv run --project tools python tools/bf_ground.py --port COM5 --log run.jsonl
```

`tools/bf_ground.py` is the PC-side ground tool and the Python mirror of the wire protocol;
`test/test_ground.py` checks that mirror against the same encoding rules as `src/service/protocol`.

### RC ground station (Wi-Fi + BetaFPV joystick)

`tools/bf_rc.py` is the **dedicated RC ground station**, independent of the bench station. It reads a
BetaFPV LiteRadio 3 over USB HID with `pygame-ce`, maps the sticks to the flight controller's
flapping command, and speaks the 60-byte protocol over UDP to the flight controller's Wi-Fi AP. See
`docs/rc-host.md`.

```bash
uv run --project tools python tools/bf_rc.py                 # PC Wi-Fi ↔ flight controller
uv run --project tools python tools/bf_rc.py --list          # list joysticks
uv run --project tools python tools/bf_rc.py --host 192.168.4.1 --port 3335
```

Session rules are the same as the ESP-NOW path: HELLO → STATUS/lease → 50 Hz CONTROL, 150 ms status
timeout, explicit ARM and ESTOP. The station never arms automatically, only sends a hold command
while disarmed, and refuses to enter flap mode before arming. Its local limits are estimates; the
flight controller's parameters remain authoritative. This link is bench-only (no app-layer auth).

### Bench console

`bf_console` registers an `esp_console` command set on the programming port (flight-controller
build only): `status`, `params`, `param get/set/reset`, `events`, `servo <l|r> <deg>` / `servo off`
/ `servo enable`, `session`, `arm`, `disarm`, `estop`, `clear`. It is the temporary bench command
path until the Phase 4 USB frame protocol lands. Write commands are refused while armed or faulted.

## Architecture

### Tasks and locking

`app_main` brings up board → parameter → telemetry → servo → sensors → safety → flight → radio,
runs a self-check, then creates three tasks:

| Task | Prio | Rate | Responsibility |
|---|---:|---|---|
| `control_task` | 8 | ~333 Hz (`CONTROL_PERIOD_US 3003`) | Per tick: deadline check → `flight_tick` → `safety_tick` → `servo_update` |
| `monitor_task` | 5 | ~100 Hz | `sensors_sample`, power/fault evaluation, parameter-validity fault |
| `telemetry_task` | 3 | 20 Hz | Publish snapshot; ~1 Hz snapshot dump to log |

The 333 Hz tick is driven by a periodic `esp_timer` that only notifies the task — the PLAN forbids
using the RTOS tick as the 1/333 s time base. A tick longer than two periods raises `BF_FAULT_TASK_WDT`
and ends the session instead of catching up.

`bf_runtime_lock()`/`bf_runtime_unlock()` (a recursive mutex, `src/bsp/common/bf_runtime.c`)
serializes all cross-task access to shared module state. Any code path that reads or mutates
parameter/servo/safety/flight/telemetry state must run inside that lock — this is the only
concurrency guard in the project.

### Layers (`src/app/`, `src/service/`, `src/driver/`, `src/bsp/`)

Build registration is per layer. Dependencies point down: app → service → driver → bsp.
`docs/architecture.md` describes ownership, configuration injection, desktop startup and migration.
The module responsibilities below remain, with flight/console in app; parameter/safety/flapping/
protocol/radio/telemetry in service; servo/sensors/espnow/ST vendor in driver; board/common in bsp.

- `common` — local CRC-32/ISO-HDLC (test vector `"123456789"` → `0xCBF43926`), fault bit vocabulary
  (`bf_fault.h`), runtime mutex. CRC is implemented locally, not from ESP-IDF, so host tools and
  target produce identical bytes.
- `board` — the only place pin numbers may appear (`board_pins.h`), plus `board_caps()` capability
  descriptor, status LED, buzzer, reset-reason string. No business code may hardcode a pin.
- `parameter` — NVS-backed parameter store: single CRC-protected blob in A/B slots with an active-slot
  pointer, schema version, range-checked writes, factory reset. `bf_param_id_t` and the descriptor
  table must stay in sync. Unmeasured values carry `calibrated = 0`.
- `servo` — MCPWM dual-channel 50 Hz from a shared time base; PWM starts with no pulses;
  `servo_math.h` holds pure angle→pulse and slew helpers (host-testable).
- `flapping` — pure trajectory generator (frequency/amplitude/center/phase/differential, ramp,
  speed/acceleration envelopes). No ESP-IDF dependency.
- `safety` — the safety state machine (`BOOT_LOCKED → READY_LOCKED → ARMED → SAFE_RETURN_LOCKED /
  LOCKED / FAULT_LATCHED`), fault latch/lower, arm preconditions, estop.
- `flight` — glues radio frames to servos: session/lease/sequence/freshness validation, ACK cache,
  STATUS emission, and the ARMED-gated drive of `flapping` → servo targets.
- `lsm6ds3tr_c` — vendored, unmodified ST official LSM6DS3TR-C driver (PID, BSD-3-Clause, pinned by
  version + commit in `vendor/` and the component README). Register-level only; the bus glue lives
  in `sensors`. Do not edit `vendor/` — re-pull upstream to update.
- `sensors` — battery/current ADC sampling and filtering; conversions come from the parameter store.
  Also the IMU/barometer sampling path (`motion.c`): implements the ST driver's
  `platform_read`/`platform_write`/`platform_delay` on SPI, and the datasheet-derived SPA06-003
  coefficient decode and compensation in `motion_math.c`. `sensors_debug.h` adds raw bus access for
  the bench firmware only — it is not part of the flight path.
- `radio` — protocol decode and receive queue with a separate bounded estop latch. The lower
  `src/driver/espnow` owns encrypted ESP-NOW unicast and MAC whitelist; `src/driver/wifi_link` owns
  the mutually exclusive Wi-Fi AP + UDP transport for the direct PC link. Both feed the same
  `radio` queues, so `flight` validation is transport-independent.
- `protocol` — the 60-byte frame codec, CONTROL/STATUS payload encode-decode, COBS.
- `telemetry` — fixed-size snapshot + bounded event ring; deliberately stores safety state as a plain
  int so it does not create a dependency cycle.
- `bf_console` — bench console commands.

### Control data path

ESP-NOW RX (or Wi-Fi/UDP RX when `CONFIG_BF_WIFI_LINK_ENABLED`) → `flight.c` validates (session
match, strictly-increasing sequence, CRC, payload ranges, TTL deadline, and a still-valid lease id
from the last STATUS) → `flapping_command`/`flapping_step` produce per-side angles →
`servo_set_target_deg` → MCPWM. Everything is gated on `safety_state() == SAFETY_ARMED` **and** a
fresh control frame; otherwise the trajectory is reset.
HELLO stops any existing session before issuing a new random session id. CONTROL frames are never
retransmitted or queued — the newest valid snapshot wins. ESTOP has its own queue so a full RX
queue can never drop it.

### Protocol

`docs/protocol.md` and `src/service/protocol` are authoritative and must agree; `tools/bf_ground.py`
is the third copy. Fixed 60-byte frame, little-endian, no raw C structs on the wire: 24-byte header
(magic `0x4642`, version, type, vehicle_id, session, sequence, request_id, payload_len, reserved),
32-byte payload, CRC-32/ISO-HDLC trailer over the first 56 bytes. USB/remote links wrap frames in
COBS with `0x00` delimiters. Message types: HELLO=1, CONTROL=2, ARM=3, DISARM=4, ESTOP=5, CLEAR=6,
STATUS=7, ACK=8. Changing any field means updating the doc, the firmware, the Python tool, and the
golden samples together.

## Non-negotiable project rules

These come from `docs/PLAN.md` and the hardware baseline; violating them is a safety regression.

- **Never arm automatically.** There is no automatic path out of a locked state: link recovery
  requires a fresh HELLO, CLEAR, CONTROL and an explicit ARM. Frames/leases/ACKs must be idempotent
  and replay-safe.
- **PWM off is not power off.** The current board has no servo power switch; `board_caps()
  ->has_servo_power_switch` is false and `board_servo_power_*()` return `ESP_ERR_NOT_SUPPORTED`.
  Never fake a cutoff by stopping PWM; report the `NO_POWER_CUTOFF` capability limit instead.
- **Unmeasured values stay parameters.** Servo pulse endpoints, divider ratio, shunt/INA199 gain,
  safe angles and voltage/current thresholds are placeholders with `calibrated = 0` — never hardcode
  them and never present them as flight limits. Arming is refused while calibration is invalid.
- **No wireless before the safety state machine.** Keep the current ordering; do not add control
  paths that bypass `safety`/`flight` gating.
- **Document before changing.** Pins, protocol fields, defaults, and safety actions must be updated
  in the relevant doc (and the version bumped) before the code changes.
- **Secrets never enter Git.** `sdkconfig` is gitignored; peer MACs and PMK/LMK belong in local
  build config only, never in a commit, log, or parameter export.

## Where to record work

Append to the `Implementation record` section of `docs/PLAN.md` (`## 10. 实施记录`) after each
stage: scope done, key decisions, build/test commands used, and what is still open. Hardware
measurements and their status live in `docs/hardware-baseline.md`. Keep the phase ordering and exit
conditions from the PLAN intact — do not implement a later phase while an earlier exit condition is
open.

`docs/diagrams/` holds generated archify HTML/JSON diagrams of the architecture; `*.visual-check.*`
are render checks. No git remote is configured for this repository.
