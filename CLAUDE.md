# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

BFCopilot is flapping-wing (bionic butterfly) flight-controller firmware for the current
butterfly PCB: an ESP32-C6 flight controller plus a second ESP32 as the remote, linked by
encrypted ESP-NOW. Written in C on **ESP-IDF v6.0.2**, target `esp32c6` (see `sdkconfig.defaults`).

Two firmwares share one project tree, selected at configure time:
- **Flight controller** (default) — entry point `main/app_main.c`.
- **Remote** — entry point `main/remote_main.c`, built with `-D BF_REMOTE=1`. It is only a bridge:
  it does not implement control logic, it converts COBS-framed USB/UART frames to ESP-NOW and back.

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
`test/test_ground.py` checks that mirror against the same encoding rules as `components/protocol`.

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

`bf_runtime_lock()`/`bf_runtime_unlock()` (a recursive mutex, `components/common/bf_runtime.c`)
serializes all cross-task access to shared module state. Any code path that reads or mutates
parameter/servo/safety/flight/telemetry state must run inside that lock — this is the only
concurrency guard in the project.

### Components (`components/`)

Each is an ESP-IDF component with `include/` + `CMakeLists.txt`. Dependency direction is one-way;
`common` and `board` sit at the bottom.

- `common` — local CRC-32/ISO-HDLC (test vector `"123456789"` → `0xCBF43926`), fault bit vocabulary
  (`bf_fault.h`), runtime mutex. CRC is implemented locally, not from ESP-IDF, so host tools and
  target produce identical bytes.
- `board` — the only place pin numbers may appear (`board_pins.h`), plus `board_caps()` capability
  descriptor, status LED, buzzer, reset-reason string. No business code may hardcode a pin.
- `parameter` — NVS-backed parameter store: single CRC-protected blob in A/B slots with an active-slot
  pointer, schema version, range-checked writes, factory reset. `bf_param_id_t` and the descriptor
  table must stay in sync. Unmeasured values carry `calibrated = 0`.
- `servo` — MCPWM dual-channel 333 Hz from a shared time base; PWM starts with no pulses;
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
  coefficient decode and compensation in `motion_math.c`.
- `radio` — encrypted ESP-NOW unicast with MAC whitelist; receive queue with a separate bounded
  estop latch.
- `protocol` — the 60-byte frame codec, CONTROL/STATUS payload encode-decode, COBS.
- `telemetry` — fixed-size snapshot + bounded event ring; deliberately stores safety state as a plain
  int so it does not create a dependency cycle.
- `bf_console` — bench console commands.

### Control data path

ESP-NOW RX → `flight.c` validates (session match, strictly-increasing sequence, CRC, payload ranges,
TTL deadline, and a still-valid lease id from the last STATUS) → `flapping_command`/`flapping_step`
produce per-side angles → `servo_set_target_deg` → MCPWM. Everything is gated on
`safety_state() == SAFETY_ARMED` **and** a fresh control frame; otherwise the trajectory is reset.
HELLO stops any existing session before issuing a new random session id. CONTROL frames are never
retransmitted or queued — the newest valid snapshot wins. ESTOP has its own queue so a full RX
queue can never drop it.

### Protocol

`docs/protocol.md` and `components/protocol` are authoritative and must agree; `tools/bf_ground.py`
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
