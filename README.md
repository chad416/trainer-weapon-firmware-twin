# Trainer-weapon firmware on a digital twin

[![ci](https://github.com/chad416/trainer-weapon-firmware-twin/actions/workflows/ci.yml/badge.svg)](https://github.com/chad416/trainer-weapon-firmware-twin/actions/workflows/ci.yml)

Zephyr RTOS firmware (C) for an instrumented training weapon. It runs on an emulated STM32H753 board (Antmicro's STM32H7 Renode Reference Platform) inside the Renode simulator, so no physical hardware is needed. A Python lane console on the host talks to the emulated weapons over CAN.

**What the weapon does:**
- reads an analog trigger, a safety selector, a smart magazine and a charging handle
- fires an emitter gate and a recoil-valve gate
- measures aim, cant and flinch with an IMU
- simulates instructor-set stoppages
- reports everything on a CAN lane bus whose protocol is defined in a DBC file

**Contents:**
- [SPEC.md](SPEC.md): specification and findings
- [docs/traceability.md](docs/traceability.md): every requirement mapped to its tests
- [docs/session/summary.md](docs/session/summary.md): report from a recorded two-weapon session

![lane console during a two-weapon session, animated](docs/media/lane_console.gif)

*The lane console through the whole recorded demo session (real output, 36 s). The same session as a video: [docs/media/lane_console.mp4](docs/media/lane_console.mp4); GitHub's file page does not play video, so use Raw to download it.*

![lane console during a two-weapon session](docs/media/lane_console_2.png)

*Lane console during the recorded demo session (real output). Weapon 2 has fired a full-auto burst at 900 rpm. Weapon 1's late stoppage clearance is flagged `CLEAR_LATE`.*

## Status

All four weeks of the plan are done.

**Tests** (firmware v0.5.0):
- **Unit tests:** 76 of 76 pass (ztest on `native_sim`).
- **Integration tests:** all 29 Robot Framework tests pass in Renode, in two consecutive runs.
- **Stress test:** the full two-weapon run, 10,000 shots each at 900 rpm, passes on the same build (17 minutes).

**Open:**
- Some requirements are only partly covered: F01 per-unit calibration, F14 fault injection beyond the IMU, R02 bus-off, and T02 measured separately. [docs/traceability.md](docs/traceability.md) has the details.
- **CI:** every push builds the firmware and runs the protocol-vector check, the unit tests and the Renode integration tests (about 5 minutes on GitHub's Ubuntu runner). The full 10,000-shot stress run is too long for CI and is run locally.

## What it does, and how it's tested

| Feature | Requirement | Tests |
|---|---|---|
| Analog trigger sampled at 1 kHz, break/reset with hysteresis, squeeze metrics | F01, F02 | unit `trigger`; IT03 |
| A trigger held at power-up can't fire; safety selector SAFE → SEMI → AUTO | R04, F03 | unit `weapon`, `trigger`; IT01, IT01b |
| Magazine, rack and chamber, including "30 + 1"; SEMI and AUTO (750 rpm) | F04–F06 | unit `rounds`, `weapon`; IT02, IT03, IT04 |
| Dry fire; emitter gate (2 ms) and recoil-valve gate (20 ms) pulses | F05, F07 | IT05, IT14 |
| Stoppages: instructor-injected, at round N, random per-mille; tap-rack drill; late clearance | F08 | unit `malfunction`; IT06, IT06a–c |
| IMU at 208 Hz: muzzle rate (300 ms), cant, flinch (50 ms) before each shot → SHOT_DETAIL | F10 | unit `imu`; IT07–IT07d replay recorded motion (RESD) |
| CAN reporting: SHOT_EVENT, SHOT_DETAIL, EVENT, 10 Hz STATUS, BIT_RESULT, INFO | F09–F11 | unit `proto` (byte-exact against the DBC); IT01, IT02 |
| On-device shot log: last 1000 shots in RAM, `trainer log [n\|clear]` | F15 | unit `shotlog`; IT16 |
| Commands over CAN (ISO-TP single frames): GET, SET (saved), RUN_BIT, INJECT, REBOOT, RESET_DEFAULTS | F13 | IT08, IT08c, IT14b |
| Configuration in flash with CRC; corrupt config → defaults + fault bit | F13, R03 | unit `cfg`; IT08, IT08b |
| Lane time from SYNC: least-squares offset and drift, bounded to ±500 ppm, wrap and restart handling | F12 | unit `timesync` (≤ 50 µs at ±200 ppm); IT10b, IT10c |
| Self-test: IMU, ADC, outputs (critical); trigger at rest, config, CAN, flash, watchdog | F14 | IT01, IT12 |
| Task watchdog on every thread, IWDG fallback | R01 | IT11, IT11b |
| Status LED: SAFE blue, not loaded slow green blink, READY green, FAULT red | F16 | IT15, IT12 |
| Fire-path timing (virtual time): emitter ≤ 1.5 ms after break, SHOT_EVENT ≤ 2 ms | T01–T03 | IT10 |
| Two weapons at 900 rpm: complete in-order shot stream, no TX overflow | T05 | IT09a, IT09b (CI: 3 magazines; `STRESS_MAGS:100` = 10,000 shots each) |

<p>
<img src="docs/media/tests_unit.png" width="49%" alt="unit test run">
<img src="docs/media/tests_sim.png" width="49%" alt="Renode integration test run">
</p>

*Captured output of the final unit and integration test runs.*

## Design in brief

```
 trigger pot ─► trigger thread (1 kHz ADC, hysteresis) ──┐
 SW1..SW4 ────► input callbacks ─────────────────────────┤  event queue
 cadence timer ──────────────────────────────────────────┤
                                                          ▼
                   control thread: stoppage decision ─► weapon state machine (Zephyr SMF)
                        │ outputs (emitter, valve, status LED)
                        │ SHOT_EVENT / SHOT_DETAIL / EVENT
 IMU thread (208 Hz) ───┘ metrics at each shot
 lane thread: CMD service, SYNC fit, 10 Hz STATUS ─► CAN TX thread ─► FDCAN1 ─► lane bus
 task watchdog: control, trigger, IMU, CAN TX, lane threads
```

```
SAFE
ARMED ── EMPTY | LOADED | READY | FIRING
STOPPAGE
FAULT
```

- **Protocol:** the CAN protocol is in [protocol/trainer.dbc](protocol/trainer.dbc), with ID = (function << 7) | node. `protocol/gen_vectors.py` encodes sample messages with `cantools` into a C header, and the firmware encoders must match it byte for byte (`--check` in CI).
- **Pure logic modules:** `trigger_proc`, `rounds`, `weapon_sm`, `malfunction`, `shotlog`, `proto`, `cfg`, `imu_metrics` and `timesync` have no hardware calls, so they are unit-tested on `native_sim`.

## Quick start (Windows 11 + WSL2 Ubuntu 24.04)

One-time setup inside WSL: the Zephyr SDK, `west`, Renode 1.17, the Robot requirements, and for the demo `can-utils`, `ffmpeg`, `librsvg2-bin`, `rich` and `matplotlib`. The CI workflow (`.github/workflows/ci.yml`) lists the exact steps.

```bash
bash scripts/build.sh       # firmware for the emulated board
bash scripts/test-unit.sh   # ztest unit tests on native_sim (twister)
bash scripts/test-sim.sh    # Robot Framework integration tests in Renode
bash scripts/demo.sh        # two weapons + lane console session -> docs/session, docs/media
```

The scripts bind-mount the repository at `~/trainer-src` when its path contains a space, because Zephyr's tools break on spaces.

### Lane console on the live simulation

`sim/lane_live.resc` bridges the emulated CAN bus to the host's SocketCAN `vcan0`. On the host:

```bash
python3 tools/lane/lane_console.py --log session.csv   # live view; sends SYNC every 10 ms
python3 tools/lane/lane_cmd.py 2 set auto_rpm 900       # one command, prints the ACK
python3 tools/lane/lane_cmd.py 1 inject                 # instructor stoppage
python3 tools/lane/session_report.py session.csv --out report/
```

### Controls on the emulated board

| Trainer | Board | Renode |
|---|---|---|
| Trigger | Potentiometer (ADC1 INP2) | `sysbus.adcM1S2.potentiometer SetPercentage` |
| Selector / magazine / rack / tap | SW1 / SW2 / SW3 / SW4 | `sysbus.gpioPortG.sw1` … `sw4` (hold > 30 ms) |
| Emitter gate / recoil valve gate | PE2 / PE3 | `sysbus.gpioPortE.emitterGate` / `valveGate` |
| Status LED | RGB on PD13–15 | `sysbus.gpioPortD.{red,green,blue}RgbLed` |
| IMU | LSM6DSO on I2C1 | `sysbus.i2c1.lsm6dso` (`Feed*Sample`, `Feed*SamplesFromRESD`) |

## Things found along the way

All of these are written up in SPEC §13:
- **Simulator clock:** Renode's STM32H7 clock model assumed an 8 MHz crystal, but this board has 25 MHz. Firmware time ran at 0.32× simulated time and debounced buttons stopped working. `sim/trainer_board.repl` sets the crystal to 25 MHz.
- **Time-sync robustness:** Renode's CAN delivery jitter pushed the drift fit to an absurd slope. The firmware now bounds drift to ±500 ppm, which also protects a real lane against a misbehaving master.
- **Protocol fix from the demo:** recorded sessions showed take-up and squeeze saturating at 255 ms, so they moved to 4 ms steps (0–1020 ms) in v0.4.
- **Screenshot renderer:** `cairosvg` drew the wrong text in one table cell (AUTO shown as SEMI), so screenshots are rendered with `rsvg-convert`.

## Honest limits

- **Timing:** Renode is functional, not cycle-accurate. The timing tests prove ordering and scheduling in firmware time, not STM32H7 latencies.
- **Reset reason:** a Renode reset clears RAM and the RCC reset flags, so the simulated reset reason after a watchdog reset reads PIN. On silicon it reads TASK_WATCHDOG.
- **Bus-off:** it can't be injected in Renode, so recovery is untested.
- **Time-sync accuracy:** Renode delivers test frames after a host-dependent delay, so absolute accuracy is proven in unit tests.
- **Two-weapon test:** Renode allows one CAN tester per bus, so IT09b reads node 2's full stream and checks node 1 through its counters.
- **Demo motion:** the IMU motion in the demo is scripted (Renode samples), not a recording of a real weapon.

## Layout

```
app/            Zephyr application (src/, overlay, Kconfig, prj.conf)
protocol/       trainer.dbc (CAN protocol), test vectors and their generator
tests/unit/     ztest suites for the pure logic modules
tests/robot/    Renode Robot Framework integration tests
sim/            Renode platform (.repl) and scripts: single, lane, lane_live (SocketCAN bridge)
sim/data/       IMU recordings (RESD) and their generator
tools/lane/     lane console, command tool, session report, demo scenario, media helpers
docs/           traceability, recorded session report, screenshots and video
scripts/        build, test and demo helpers
.github/        CI: build, unit tests, Renode integration tests, report artifacts
west.yml        pins Zephyr to the reference platform's revision
```

## Licence notes

`sim/trainer_board.repl` contains Antmicro's board description from the Zephyr tree (Apache-2.0), with the changes marked in the file.
