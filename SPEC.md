# Trainer-Weapon Firmware on a Digital Twin: Specification

Revision C (8 Nov 2026), firmware v0.6.2.
- Revision B added F17, camera mode (firmware v0.6.0); v0.6.1 fixed the strobe pulse width (§13.6).
- Revision C describes the firmware as built (§5, §6, §9), restates F09, F11, F14, F15, F17, R01 and R02 to match the firmware, and adds the v0.6.2 fixes (§13.7, [CHANGELOG.md](CHANGELOG.md)).

Embedded C firmware for an instrumented training weapon, on Zephyr RTOS. It runs on an emulated STM32H753 board (Antmicro's STM32H7 Renode Reference Platform) inside the Renode simulator, with unit tests, integration tests and CI. It has not been run on hardware.

---

## 1. Context

The firmware is one part of a simulated training lane. The other parts are separate repositories: [motion-platform-controller](https://github.com/chad416/motion-platform-controller) and [aim-point-tracker](https://github.com/chad416/aim-point-tracker), the lane camera that finds where each weapon is aimed and where it hit. The weapon talks to the lane over CAN, and in camera mode (F17) times its emitter flashes to the tracker's camera exposures.

An instrumented trainer weapon:
- matches the real weapon's mass and balance
- has a non-contact trigger sensor
- uses "smart" magazines that count rounds and give CO2 recoil
- talks to the lane over a wireless link
- supports instructor-injected malfunctions that the trainee has to clear
- produces after-action data: aim movement, firing rhythm, reloads, malfunction handling

This firmware models all of these. The link from the weapon to the lane is modelled as **CAN**, standing in for the link of a belt-worn interface module (a radio is future work, §12).

---

## 2. Scope

### In scope
- One Zephyr application, `trainer_fw`, for board `stm32h7_renode_reference_board`.
- Renode setups for one weapon, and for two weapons plus a lane bus.
- Unit tests (ztest on `native_sim`) and integration tests (Robot Framework + Renode).
- GitHub Actions CI.
- A DBC file and a Python lane console.

### Out of scope
- Real hardware
- Camera aim tracking (the aim-point-tracker repository)
- Wireless radio (§12)
- Silicon-accurate timing (§9.4)

---

## 3. Mapping the trainer onto the emulated board

All of these peripherals exist in the board's devicetree and in the Renode model.

| Trainer function | Board resource | How the simulator drives it |
|---|---|---|
| Analog trigger travel | Potentiometer on ADC1 INP2 (PF11) | `SetPercentage` from the monitor or Robot (ramps = squeezes) |
| Safety selector: SAFE → SEMI → AUTO | SW1 (PG7), each press cycles | Button Press/Release |
| Magazine insert/remove | SW2 (PG8), toggles | Button Press/Release |
| Charging handle (rack) | SW3 (PG9) | Button Press/Release |
| Tap (malfunction drill) | SW4 (PG10) | Button Press/Release |
| Shot emitter output (laser/IR gate) | GPIO PE2 (free pin) | Simulated LED `emitterGate` in `sim/trainer_board.repl`, read by the LED tester |
| Recoil valve output (CO2 solenoid gate) | GPIO PE3 (free pin) | Simulated LED `valveGate`, read by the LED tester |
| Status indication | RGB LED (PD13–15) | Read by the LED tester |
| Aim, cant, flinch | LSM6DSO IMU on I2C1 | Data replayed from RESD files |
| Lane link | FDCAN1, classic CAN 500 kbit/s | Renode CAN hub (plus a SocketCAN bridge on Linux) |
| Configuration | Settings on NVS in the QSPI storage partition | Survives `machine Reset` and reboots (IT08) |
| Shot log | RAM ring buffer (F15) | Lost on reset, as specified |
| Debug | USART2 console + Zephyr shell | Renode UART analyzer |

Pins are chosen in the app devicetree overlay (`app/boards/stm32h7_renode_reference_board.overlay`) through aliases (`trainer-emitter`, `trainer-valve`, `trainer-status-red`/`-green`/`-blue`; the trigger ADC channel is `zephyr,user` `io-channels`). A different board needs a new overlay, not code changes.

The board's green LEDs sit on TIM3 PWM and have no LED model in Renode, so the emitter and valve gates are on the free GPIOs PE2/PE3. The trainer's Renode platform (`sim/trainer_board.repl`) attaches LED models to those pins.

---

## 4. Requirements

### 4.1 Functional
| ID | Requirement |
|---|---|
| F01 | Sample trigger travel at 1 kHz. Apply per-unit calibration (rest, take-up, break, full travel) stored in settings. |
| F02 | Detect trigger break when travel crosses the break threshold. Re-arm only after travel falls below the reset threshold (hysteresis). |
| F03 | Power-up state is SAFE. SW1 cycles SAFE → SEMI → AUTO → SAFE. |
| F04 | Magazine: inserting a magazine loads a configurable capacity (default 30). Rack chambers a round. Removing the magazine keeps the chambered round (30 + 1 behaviour). |
| F05 | Fire only when not SAFE, a round is chambered and no stoppage is active. Each shot pulses the emitter and the recoil valve, decrements the rounds, and auto-chambers the next round if the magazine has one. |
| F06 | SEMI: one shot per break. AUTO: re-fire at the configured cadence (600–900 rpm) while travel stays past break. |
| F07 | Trigger break with no chambered round gives a dry-fire event: no emitter, no valve. |
| F08 | Malfunction simulation: the instructor injects a stoppage now, at round N, or with probability p per shot. The weapon enters STOPPAGE and fire is blocked. Clearance is the drill tap (SW4), then rack (SW3), within a configurable window. Report the clearance time. |
| F09 | Send a SHOT_EVENT on CAN for every shot, stamped with the time of the trigger sample that crossed the break point (AUTO: the cadence tick), in lane time once synchronised. |
| F10 | Send SHOT_DETAIL with trigger metrics (take-up time, squeeze duration, jerk score) and IMU metrics (mean angular rate over 300 ms before break, cant angle at break, flinch = pitch change in the last 50 ms). |
| F11 | Send STATUS at 10 Hz: state, selector, magazine, rounds, chambered, stoppage, fault bits, TX overflow and TX error counters, shot count. (Revision B listed uptime; STATUS carries the shot count instead, and uptime is in the shell's `trainer status`.) |
| F12 | Lane time synchronisation: a SYNC frame from the lane carries lane time. The node estimates offset and drift and stamps events in lane time. |
| F13 | Configuration over CAN and the shell: node ID, capacity, cadence, thresholds, pulse widths, malfunction settings. Stored with settings/NVS and checked by CRC. |
| F14 | BIT at boot and on request (RUN_BIT over CAN, `trainer bit`): outputs and ADC initialised, IMU answering (the driver reads WHO_AM_I at start, and the IMU thread read a good sample within the last 50 ms), trigger at rest, stored configuration valid (CRC), CAN controller error-active, no settings read/write error, every watched thread holding a watchdog channel. Send BIT_RESULT. Critical faults (outputs, ADC, IMU) block fire. |
| F15 | Shot log: RAM ring buffer of the last 1000 shot records, lost on reset. Dumped through the shell; segmented CAN transfer is future work (§12). |
| F16 | Status LED: SAFE = blue, READY = green, STOPPAGE = amber blink, FAULT = red, EMPTY = slow green blink. |
| F17 | Camera mode (v0.6), for the lane camera of the aim-point tracker. With STROBE_SLOTS = 2–4 and STROBE_SLOT set over CAN (session keys 13, 14), the emitter flashes 0.5 ms in the middle of the camera's exposures (exposure k: lane time k × 10 ms + 4.5 ms, 1 ms long), on the node's lane time (F12): an aim pulse in each exposure with k mod STROBE_SLOTS = STROBE_SLOT, and at a trigger break the shot pulse in the first exposure whose pulse starts at least 2 ms after the break, replacing that frame's aim pulse. The recoil valve still fires at the break. One slot is refused: the tracker needs a dark frame between a weapon's flashes. Without lane time the shot pulse fires at once and is counted as unsynced. STROBE_SLOTS = 0 (the default) is direct mode: the emitter pulses at the break as in v0.5. |

### 4.2 Robustness
| ID | Requirement |
|---|---|
| R01 | A task watchdog covers the control, trigger, IMU, CAN TX, lane and strobe threads. A stalled thread causes a reset with the reason logged. |
| R02 | CAN bus-off: automatic recovery with back-off. A frame that cannot be queued (TX queue overflow) or sent (3 attempts) is counted, and both counters are reported in STATUS: no frame is dropped silently. |
| R03 | Corrupt settings fall back to defaults, set a fault bit, and are logged. |
| R04 | A reset never fires a shot. Outputs default to off in the devicetree, and the state machine always starts in SAFE. |

### 4.3 Timing (measured in emulated virtual time, see §9.4)
| ID | Requirement |
|---|---|
| T01 | Trigger break detected within one sample period (≤ 1 ms after the ADC crosses the threshold). |
| T02 | Emitter and valve outputs set ≤ 200 µs after detection. In camera mode (F17) this holds for the valve; the emitter flashes in the shot frame instead. |
| T03 | SHOT_EVENT on the bus ≤ 2 ms after break. |
| T04 | AUTO cadence within ±2 % of the set rpm. |
| T05 | Two weapons at 900 rpm on one bus: no lost frames over 10 000 shots. |

---

## 5. Firmware architecture

```
 ADC, 1 kHz timer ──► trigger (prio 2): travel, break/reset ─────┐
 SW1..SW4 (gpio-keys) ──► input callback ─────────────────────────┤  event queue (k_msgq);
 AUTO cadence (k_timer) ──────────────────────────────────────────┤  each event carries the
                                                                   ▼  time it was sampled
              control (prio 3): stoppage decision ─► weapon state machine (Zephyr SMF)
                 │ direct mode: emitter + valve gate pulses (k_timer one-shots)
                 │ camera mode: valve now; shot frame ─► strobe (prio 2) ─► emitter window
                 │ SHOT_EVENT, SHOT_DETAIL, EVENT, shot log, status LED (work queue)
 IMU (prio 5, 208 Hz): ring buffer ─► metrics at each shot
 lane (prio 6): CMD service, SYNC fit, 10 Hz STATUS ─► TX queue ─► can_tx (prio 4) ─► FDCAN1
 BIT (sys/bit.c): at boot, on RUN_BIT and from the shell
 task watchdog (sys/wdt.c): control, trigger, imu, can_tx, lane, strobe; IWDG fallback
```

Design choices:
- **Zephyr State Machine Framework (SMF)** for the weapon states. It is hierarchical: ARMED contains EMPTY, LOADED, READY and FIRING.
- **Message queues** carry events from ISRs and threads to the control thread (`event_q`), frames to the CAN TX thread (`tx_q`), and CMD and SYNC frames to the lane thread. Other threads read shared state through snapshot functions under a spinlock or mutex.
- **Devicetree aliases and Kconfig** for every hardware dependency and tunable.
- **Fire outputs** are switched in the control thread as soon as the state machine returns the fire action. Pulse widths come from `k_timer` one-shots; the camera-mode window switches both gate edges in the timer interrupt (§13.6).
- **Pure logic modules** (`trigger_proc`, `rounds`, `weapon_sm`, `malfunction`, `shotlog`, `proto`, `cfg`, `imu_metrics`, `timesync`, `strobe`) have no Zephyr device calls, so they are unit-tested on `native_sim`.

### 5.1 Weapon state machine
States:
- `SAFE`
- `ARMED`, containing `EMPTY`, `LOADED` (magazine in, nothing chambered), `READY` (chambered) and `FIRING`
- `STOPPAGE`
- `FAULT`

Rules:
- `SAFE` is entered from anywhere when the selector is on SAFE. Rounds are preserved.
- `STOPPAGE` is entered from `READY` or `FIRING`, when the instructor injects a stoppage or a round attempt turns into one (F08). It is left only by the correct tap-then-rack sequence.
- `FAULT` is entered on a critical BIT fault and is left only by a BIT pass.
- Every transition is logged (`STATE ...`) and reported as an EVENT of type STATE.

### 5.2 Modules
| Module | File(s) | Responsibility |
|---|---|---|
| trigger_proc | `src/trigger/trigger_proc.c` | Calibration, threshold detection with hysteresis, squeeze metrics |
| trigger_task | `src/trigger/trigger_task.c` | 1 kHz ADC sampling thread |
| weapon_sm | `src/weapon/weapon_sm.c` | SMF states, fire decisions, cadence actions |
| rounds | `src/weapon/rounds.c` | Magazine and chamber logic (30 + 1) |
| malfunction | `src/weapon/malfunction.c` | Injection modes, clearance window |
| shotlog | `src/weapon/shotlog.c` | Ring buffer of the last 1000 shots |
| control | `src/control/control.c` | Event queue, state machine, outputs, shot reports |
| inputs, outputs | `src/io/` | Buttons to events; emitter, valve and status LED |
| imu | `src/imu/` | Zephyr sensor API for the LSM6DSO, 500 ms ring buffer, pre-shot metrics, health for BIT |
| proto | `src/proto/` | CAN encode/decode, hand-written and tested byte for byte against the DBC |
| canlink | `src/can/canlink.c` | TX queue and thread (with retry), RX filters, bus state |
| lane | `src/can/lane.c` | Reports, CMD/CMD_ACK service, SYNC fit, STATUS |
| timesync | `src/timesync/` | Offset/drift estimate from SYNC |
| strobe | `src/strobe/` | Camera-mode schedule (pure logic) and the strobe thread |
| bit, wdt | `src/sys/` | Self-tests and fault mask; task watchdog and reset reason |
| cfg | `src/cfg/` | Settings handlers, defaults, CRC |
| shell_cmds | `src/shell/` | `trainer status`, `cfg`, `log`, `bit`, `can`, `imu`, `sync`, `strobe`, `inject`, `version`; with `app/debug.conf` also `stall` and `cfg corrupt` |

---

## 6. CAN protocol

- **Bus and IDs:** 11-bit IDs at 500 kbit/s, laid out as `function (4 bits) | node ID (7 bits)`.
- **Definition:** the source of truth is `protocol/trainer.dbc`. The Python side reads it with `cantools`.

| Frame | Function | Dir | Rate | Signals |
|---|---|---|---|---|
| SYNC | 0x1 | lane → all | 100 Hz | ISO-TP single frame: PCI 0x04, lane_time_us (u32) |
| SHOT_EVENT | 0x2 | node → lane | per shot | shot_no (u16), t_us (u32, break sample time), mode (2 bits), chambered (1 bit), rounds_left (u8) |
| SHOT_DETAIL | 0x3 | node → lane | per shot | shot_no (u16), takeup and squeeze (4 ms steps), jerk (u8), muzzle_rate deg/s (u8), cant deg (i8), flinch 0.1 deg (i8). One classic frame, saturating, rather than CAN-FD. |
| STATUS | 0x4 | node → lane | 10 Hz | state, sel, mag_in, chambered, stoppage, tx_errors (3 bits, saturating), rounds, faults (u16), tx_overflows (u8, saturating), shots (u16) |
| EVENT | 0x5 | node → lane | on event | type (dry fire, mag in/out, rack, stoppage start/cleared, selector, tap, state, late clearance), t_us, value |
| BIT_RESULT | 0x6 | node → lane | on event | fault_mask (u16), critical (1 bit) |
| CMD | 0x7 | lane → node | on demand | ISO-TP single frame: opcode (GET, SET and save, RUN_BIT, INJECT_STOPPAGE, REBOOT, RESET_DEFAULTS), key, value |
| CMD_ACK | 0x8 | node → lane | reply | ISO-TP single frame: opcode, key, status, value |
| INFO | 0x9 | node → lane | on boot | firmware version (major, minor, patch), reset reason, boot count |

The unit suite `proto` encodes each frame in C and compares the bytes with vectors that `protocol/gen_vectors.py` generates from the DBC with cantools. `gen_vectors.py --check` fails CI if the generated header is stale, so the firmware and the DBC cannot drift apart.

---

## 7. Simulation setup (Renode)

### 7.1 Scripts
- `sim/trainer_board.repl`: the board's Renode description (from the Zephyr tree, Apache-2.0) with the HSE fix and the emitter/valve LED models (§13.1).
- `sim/single.resc`: one weapon. Loads the platform above and `zephyr.elf`.
- `sim/lane.resc`: two weapon machines (node IDs 1 and 2) on one CAN hub, based on the multinode demo in Antmicro's reference repository.
- `sim/lane_live.resc`: `lane.resc` plus a bridge from the CAN hub to the host's SocketCAN `vcan0`.
- `sim/data/*.resd`: IMU recordings (steady aim, shaky aim, flinch before break, cant), made with `sim/data/gen_imu_profiles.py`.

### 7.2 Lane side
- **Live (Linux or WSL2):** `machine CreateSocketCANBridge` connects the hub to `vcan0`; the Python lane console, `lane_cmd.py` and Wireshark use `vcan0`.
- **Tests:** Robot sends and checks frames through Renode's CAN tester keywords, so CI needs no `vcan`.

---

## 8. Tooling and environment

- **Host:** Ubuntu 24.04 (CI runner, or WSL2 on Windows 11), Python 3.12.
- **Zephyr:** pinned in `west.yml` to revision `c605ea46` (4.4.99), the revision Antmicro's demo for this board uses. Zephyr SDK 1.0.1 (`arm-zephyr-eabi`).
- **Renode:** 1.17.0 (portable Linux package).
- **Python packages:** `requirements.txt`, plus Renode's `tests/requirements.txt` for Robot Framework.
- **Build:** `scripts/build.sh` (test image: `prj.conf` + `debug.conf`; `TRAINER_IMAGE=field` for `prj.conf` alone).
- **Unit tests:** `scripts/test-unit.sh` (`west twister -p native_sim -T tests/unit`).
- **Integration tests:** `scripts/test-sim.sh` (`renode-test tests/robot/trainer.robot`).
- **Run by hand:** `renode sim/single.resc` with `$elf` set to the built `zephyr.elf`.

---

## 9. Verification

### 9.1 Unit tests (ztest on native_sim)
| Suite | Covers |
|---|---|
| trigger | F01, F02, R04: conversion, calibration validation, hysteresis edges, squeeze metrics, trigger held at power-up |
| weapon | F03–F08: transitions, AUTO cadence, SAFE preserving rounds, stoppage drill |
| rounds | F04: insert/remove/rack permutations, the 30 + 1 case, empty magazine |
| malfunction | F08: each injection mode, clearance window |
| imu | F10: muzzle rate, cant, flinch on synthetic motion |
| timesync | F12: synthetic offset and ±200 ppm drift, convergence, error ≤ 50 µs |
| proto | §6: every frame byte for byte against the DBC vectors; STATUS tx_errors saturation |
| cfg | F13, R03: limits, CRC failure → defaults |
| shotlog | F15: wrap keeps the last 1000 in order |
| strobe | F17: slots, shot frame rule, late shot, inverse of the SYNC fit |

Coverage is not measured yet.

### 9.2 Integration tests (Robot Framework + Renode)
| Test | Covers | Scenario |
|---|---|---|
| IT01 boot | F03, F14, F16, R04 | Boot → SAFE, blue LED, BIT_RESULT clean, INFO frame with version 0.6.2, no output pulses. IT01b: trigger in SAFE fires nothing. |
| IT02 full cycle | F04–F06, F09 | Insert magazine, rack, SEMI, 30 squeezes → 30 SHOT_EVENTs, rounds count down, EMPTY after the last |
| IT03 semi reset | F02 | Break → partial release above reset threshold → break again: only one shot |
| IT04 auto cadence | F06, T04 | AUTO at 750 rpm, trigger held → intervals 80 ± 1 ms |
| IT05 dry fire | F07 | Break with an empty chamber → EVENT dry fire, outputs stay off |
| IT06 stoppage drill | F08 | Stoppage at round N, random extremes, wrong clearance order, late clearance (IT06, IT06a–c) |
| IT07 IMU metrics | F10 | RESD replay: flinch ≈ 1.2° (IT07), steady ≈ 0 (IT07b), shaky raises the muzzle rate (IT07c), canted reads 10° (IT07d) |
| IT08 config persist | F13, R03 | Capacity 20 over CMD → reboot → still 20; corrupt the settings → defaults + fault bit (IT08b); bad commands rejected (IT08c) |
| IT09 two weapons | T05, F09, R02 | `lane.resc`: IT09a node IDs; IT09b both at 900 rpm, node 2's SHOT_EVENT stream read in full while node 1 shares the bus, both nodes' shot counters, no TX overflow and no TX error. CI runs 3 magazines (300 shots each); `--variable STRESS_MAGS:100` runs 10 000. |
| IT10 timing | T01–T03, F12 | Emitter on ≤ 1.5 ms after the pot crosses break, SHOT_EVENT ≤ 2 ms (IT10); lane-time stamping exact to 2 µs (IT10b); under SYNC jitter the drift stays bounded and lane time follows it over 1 s without SYNC (IT10c) |
| IT11 watchdog | R01 | `trainer stall control` / `trigger` (test image only) → reset, stalled thread logged |
| IT12 BIT fault | F14 | IMU removed from the platform → FAULT, red LED, fire blocked, RUN_BIT still reports it |
| IT13 bus-off | R02 | Not run: Renode's CAN tester cannot inject bus errors |
| IT14 shot pulses | F05, F13 | One shot pulses the emitter gate (2 ms) and the valve gate (20 ms); a valve width set over CAN applies to the next shot (IT14b) |
| IT15 status LED | F16 | SAFE blue, EMPTY slow green blink, READY steady green, STOPPAGE amber blink |
| IT16 shot log | F15 | `trainer log n` matches the SHOT_EVENT lane times; `trainer log clear` |
| IT17 camera mode | F17, F13 | IT17: two slots, slot 0: about 10 aim pulses in 0.2 s, each starting 4.75 ms after a 20 ms lane-time boundary, within 100 µs. IT17b: the valve fires at the break, and the shot pulse goes into the exposure the tracker's rule gives for the SHOT_EVENT time. IT17c: one slot, or a slot outside the count, is refused. IT17d: the LED tester pauses on each gate edge; 10 pulses in a row are each 500 ± 25 µs wide. IT17e: a shot before any SYNC flashes at once, is counted as unsynced, and does not enter the timing error. |

### 9.3 CI (GitHub Actions, `.github/workflows/ci.yml`)
1. Fetch Zephyr through `west` (cached) and install the SDK.
2. Lint the Python tools (`ruff`).
3. Check that the protocol vectors match the DBC (`gen_vectors.py --check`).
4. Build the test image and the field image (no debug commands).
5. Run the unit tests (twister on `native_sim`).
6. Run the Robot suite in Renode.
7. Upload the Robot report and logs, the twister results, and the test image's `zephyr.elf` and map as artifacts.

The full 10 000-shot run of IT09b takes about 20 minutes and is run locally (passed on v0.6.2, traceability T05).

### 9.4 Limit of simulation
Renode runs instructions functionally, not cycle-accurately. The timing tests (IT10, IT04, IT17) prove **scheduling logic and ordering in virtual time**, not STM32H7 latencies. Real numbers need the same firmware on hardware.

---

## 10. Lane console (Python)

- `tools/lane/lane_console.py` (on `vcan0`) sends SYNC every 10 ms, decodes every frame with the DBC, and shows:
  - a live table per weapon: state, selector, rounds, stoppage, faults
  - a scrolling shot log with the detail metrics, and the event log
- `tools/lane/lane_cmd.py` sends one command (GET, SET, RUN_BIT, INJECT, ...) and prints the ACK.
- `tools/lane/session_report.py` turns a session log into a summary and PNG plots: shot timeline per weapon, trigger squeeze, IMU metrics per shot.

---

## 11. Evidence in this repository

- [docs/traceability.md](docs/traceability.md): each requirement → tests → status.
- [docs/session/](docs/session/): a recorded two-weapon session (CSV, summary, plots), made by `scripts/demo.sh`.
- [docs/media/](docs/media/): screenshots and a video of the lane console during that session, and captured test runs.

---

## 12. Future work (not built)

- Wireless link on a second emulated radio MCU.
- CO2 gas budget per magazine (pressure drop per shot, cold-temperature penalty).
- Segmented shot-log download over CAN.
- MCUboot with signed images.
- The same firmware on a real board, for real latencies.

---

## 13. Platform facts and findings

Facts checked on the pinned versions:
- Board name: `stm32h7_renode_reference_board` at Zephyr `c605ea46` (4.4.99).
- Renode commands: `sysbus.adcM1S2.potentiometer SetPercentage <0-100>`; buttons `sysbus.gpioPortG.sw1..sw4 Press` / `Release`. A press must be held longer than the gpio-keys 30 ms debounce, so `PressAndRelease` does not work.
- LSM6DSO RESD replay works on this platform (`FeedAccelerationSamplesFromRESD` / `FeedAngularRateSamplesFromRESD` with `CurrentVirtualTime`); IT07–IT07d use it.
- Stored settings survive a reset: the settings live in the QSPI storage partition, and its Renode memory survives `machine Reset` and firmware reboots (IT08).
- `modprobe vcan` works in WSL2 (kernel 6.18), so the live bridge (§7.2) works there.
- Renode CAN keywords: `Create CAN Tester`, `Wait For Frame With Id`, `Send ISOTP Message`, `Wait For ISOTP Message Hex`. There is no keyword for injecting bus errors, so IT13 (bus-off) is not run; the recovery path is Zephyr's automatic mode.
- The LED tester resolves the 2 ms emitter and 20 ms valve pulses (IT14).

### 13.1 Simulator and board
- **Simulator clock bug (fixed):** Renode's STM32H7 RCC model defaults the HSE crystal to 8 MHz, but the board has 25 MHz, and the board's Renode description does not set it. With the firmware's PLL settings, Renode set SysTick to 153.6 MHz instead of 480 MHz, so firmware time ran at 0.32× virtual time and debounced buttons stopped working. Fix: `sim/trainer_board.repl` (the board description plus `rcc: hseFrequency: 25000000`). Confirmed: SysTick reads 480 MHz after boot, and 1001 trigger samples arrive in 1002 ms.
- **Logging latency:** Zephyr's deferred logger flushes once a second by default. It is set to wake on every message so test harnesses see events promptly.
- **Robot gotcha:** text starting with `name=` is read as a named argument (e.g. `state=SAFE`), and a backslash escape does not survive Renode's remote keywords. Pass it as `content=state=SAFE ...`.
- **Paths:** Zephyr builds, twister and `renode-test` all break on a path that contains a space. If the checkout path has one, the scripts bind-mount the repository at `~/trainer-src` (`scripts/wsl-mount.sh`).

### 13.2 Protocol and CAN tester
- **CMD/CMD_ACK are ISO-TP single frames** (byte 0 = PCI). Standard diagnostic tools can talk to a node, and Renode's CAN tester can send and await them deterministically, without a host CAN interface. That keeps CI free of `vcan`.
- **DBC is the source of truth:** `protocol/gen_vectors.py` encodes sample messages with cantools into `tests/unit/src/proto_vectors.h`, and the C encoders must match byte for byte.
- **Default node ID** comes from the MCU unique ID (1 + byte sum mod 127). Renode's system-flash model holds a fixed fake unique ID (it gave node 111), so the simulator scripts pin it: zeros give node 1, and `lane.resc` gives weapon 2 node 2. Variables set in a `.resc` did not reach the included script reliably, so the values are literals.
- **Renode CAN tester behaviour:** all frame IDs share one queue. A wait consumes every frame up to its match, and a live match clears the queue, so tests wait for frames in send order. In Renode 1.17, `SendFrame` also resumes a paused emulation, so a reply can arrive in a window where the wait misses it. The reply stays queued, so `Send Cmd` recovers with a short second wait. A 200-command stress run confirmed the firmware received every frame (`rx_cmd=200`, `rx_drop=0`).
- **Reset reason in simulation:** a Renode reset clears RAM (where the task-watchdog note is kept) and does not keep RCC software-reset flags, so after a watchdog or commanded reboot the simulated reset reason reads PIN. On silicon, the `__noinit` note and RCC_RSR give TASK_WATCHDOG and SOFTWARE. IT11 therefore checks the WATCHDOG log line and the boot counter.

### 13.3 IMU, stoppages and time sync
- **IMU in Renode:** the LSM6DSO model's `Default*` properties do not change the readings in the mode Zephyr uses, but `Feed*Sample` and the RESD feeds do. RESD units are micro-g for acceleration and 1e-5 rad/s for angular rate. `sim/data/gen_imu_profiles.py` builds four recordings (steady, shaky, flinch, canted) with Renode's `csv2resd`.
- **Flinch integration** uses the rectangle rule from the oldest sample inside the 50 ms window to the shot, so it can under-count by up to one sample period (about 5 ms at 208 Hz).
- **Stoppage modes** are session settings over CAN (keys 9–12, not saved to flash). The control thread turns a round attempt into a stoppage before the state machine sees it, so `weapon_sm` stays unchanged and its tests still apply. Randomness comes from xorshift32 seeded from the cycle counter: training realism, not security.
- **Time sync:** least squares over the last 32 SYNC points (about 310 ms), with the drift bounded to ±500 ppm. The bound was added after Renode's delivery jitter produced an absurd slope. A jump over 10 ms restarts the fit, and 32-bit lane wraps are unwrapped. Unit tests show ≤ 50 µs error at ±200 ppm with 5 µs jitter when predicting 100 ms ahead.
- **Renode limits for timing tests:** frames sent by the CAN tester reach the node after a host-dependent delay (tens of ms of virtual time), and only one CAN tester can exist per hub. IT10b therefore uses one SYNC (pure offset) and checks the on-target conversion, IT10c checks the drift bound under that jitter, and IT09b reads one node's full stream.
- **SHOT_DETAIL** is one classic 8-byte frame with saturating fields, rather than CAN-FD, so the protocol stays readable on any CAN 2.0 tool.

### 13.4 Lane console and demo
- **Live bridge:** `sim/lane_live.resc` connects the emulated CAN hub to the host's SocketCAN `vcan0` (`machine CreateSocketCANBridge`; there is no `emulation` variant). `scripts/demo.sh` runs a scripted two-weapon session through Renode's monitor socket. The scripted session is 36 s long.
- **Protocol change from the first recording:** take-up and squeeze saturated at 255 ms, too short for slow aimed squeezes. SHOT_DETAIL carries them in 4 ms steps (0–1020 ms) since v0.4.
- **Monitor latency:** a monitor client that waited for each command's output added about 0.25 s per command and distorted the scripted squeezes. Commands are fire-and-forget, with output drained between steps.
- **Renode IMU model:** outside FIFO mode it holds the latest fed sample, and the repeat count has no effect. A scripted motion therefore lasts until the next feed.
- **Screenshot rendering:** `cairosvg` drew the wrong text in one cell of a correct SVG (an AUTO shot shown as SEMI), so all PNGs are rendered with `rsvg-convert`.
- **F15 closed in v0.5.0:** the traceability review found the on-device shot log missing. It is a 1000-record RAM ring buffer (16-byte records, 16 KB), filled from the same data as SHOT_EVENT and SHOT_DETAIL and dumped with `trainer log [n|clear]`. Covered by unit `shotlog` and IT16.
- **IT10b test bug:** IT10b took the shot's local time from the log timestamp, which has only ms resolution here (the µs field is always `,000`). It passed while shots landed on ms boundaries and failed with 900 µs error once they didn't. The SHOT log line prints the exact `t_local` in µs, and IT10b checks lane time = local + offset to within 2 µs. It measures 0 µs.

### 13.5 Camera mode (v0.6.0)
- **Why:** the aim-point tracker films the screen at 100 fps with 1 ms exposures. A 2 ms emitter pulse at a random time lands fully inside an exposure almost never, so the weapon has to put its pulses into the exposures (F17).
- **Margin from the break:** the tracker's first draft had the shot pulse 0.5 ms after the break. The firmware only sees a break up to 1 ms after it happens (1 kHz sampling) and fires within 1.5 ms (T01), so a 0.5 ms margin would have missed its own frame. It is 2 ms on both sides. Optical latency is at most one frame more; the valve is unchanged.
- **Timing:** pulses are planned on lane time and slept to with the inverse of the SYNC fit (`ts_local_time`). Kernel timeouts round up to the 100 µs tick, so the thread aims 50 µs early. In Renode the pulses fired 68 µs after their planned start, and a shot pulse that had to re-plan when the break arrived fired 168 µs late (one more tick). The pulses were also too long; fixed in v0.6.1.
- **Watchdog:** the strobe thread is on the task watchdog like the others (6 channels).

### 13.6 Strobe pulse width (v0.6.1)
- **Found by:** the aim-point tracker's end-to-end test (its IT10), which runs v0.6.0 on two Renode machines and replays the emitter edges into the camera. The pulses were 563–610 µs long, not 500 µs, and started −30 to +236 µs from plan. With a late start, 8 of 306 pulses ended up to 80 µs after the 1 ms exposure.
- **Why the pulse was ~90 µs long:** the strobe thread turned the gate on and started a `k_timer` for `K_USEC(500)`. A relative timeout started from a thread is rounded up by one tick (Zephyr 4.4 `kernel/timeout.c`, `z_add_timeout`: "at least N ticks" from somewhere inside the current tick). With the 100 µs tick that is 6 tick edges after the edge the thread woke on, minus the wake-up latency: 500–600 µs plus interrupt latency.
- **Why starts leaned late (+68 µs node 1, +37 µs node 2):** the wake-up was a relative timeout too: rounded up to the tick, plus the extra tick, minus the 50 µs the thread aimed early. On top of that, SYNC frames were stamped with the 100 µs tick clock (`k_uptime_ticks`, floored), so the lane-time estimate itself was off by up to a tick, in runs of many frames (seen as −144 to +182 µs with the width fixed).
- **Fix:** (1) Both gate edges are switched in the system timer interrupt on absolute tick edges (`outputs_emitter_window`): the on-timer turns the gate on and starts the off-timer for the tick 5 ticks later. Absolute timeouts get no extra tick, and both edges have the same interrupt latency, so the window is 500 µs. The strobe thread wakes 2 ticks early only to arm it. (2) Local time (`lane_local_us`, the SYNC timestamp) comes from the 64-bit cycle counter instead of the tick (`local_clock.h`), which counts from the same origin, so tick N starts at local N × 100 µs. (3) The window starts on the tick edge nearest the planned start: within ±50 µs. Finer would need a faster tick or a hardware timer channel for the gate.
- **Result in Renode:** IT17d measures 10 pulses in a row at 500 µs (LED tester, emulation paused on each edge; in each of the two full-suite runs one of the ten measured 488 µs), and the firmware's own start error is ≤ 29 µs. The limits in IT17 and IT17d are ±100 µs for the start and 500 ± 25 µs for the width; against v0.6.0, IT17d fails at 590 µs. In four runs of the tracker's two-machine co-simulation (10 µs quantum, which coarsens interrupt timing) the pulses were 473–526 µs, no pulse went past the exposure, and the mean start error was +11 to +47 µs per weapon (v0.6.0: +37 and +68).
- **Still open:** in some co-simulation runs one weapon has a stretch of pulses 100–200 µs late (worst seen +200 µs), at a different time in each run of the same firmware and script, once before any shot, so it is not tied to shots. Most likely SYNC delivery jitter between the two Renode machines pulls the time-sync fit (§13.3 saw the same kind of delivery delay). It stays inside the ±250 µs the exposure allows; the tracker's IT10 checks that limit.
- **Direct mode** (`outputs_emitter_pulse`, the 2 ms shot pulse of v0.5) still uses a relative timeout and is up to one tick longer than configured. It has no exposure to fit into, and IT14 only checks that the gate goes off within 4 ms, so it is left as it is.

### 13.7 Review fixes (v0.6.2)
A code review of v0.6.1 found these; each fix is minimal and tested where the simulator allows.
- **Shot time (F09, F17):** SHOT_EVENT carried the time the control thread handled the break, after the event queue and the state machine. Events now carry the time they were sampled: the ADC sample that crossed the break point, or the cadence tick in AUTO. SHOT_EVENT, the camera-mode shot frame, the IMU metrics window and the shot log all use it. In Renode the SHOT log line's `path_us` (sample to log line, an upper bound for the shift) is 55–57 µs for trigger breaks and 38–39 µs for AUTO ticks, so timestamps moved earlier by less than that. The tracker applies the same frame rule to the SHOT_EVENT time, so both sides still agree; pulse widths and starts are unchanged (IT17d: 500 µs, with an occasional 488 µs pulse as in v0.6.1; start error ≤ 35 µs in IT17, IT17b, IT17d).
- **Camera mode without lane time (F17):** a shot before the first SYNC flashed at once with a planned start of 0, so `max_err_us` held the whole timestamp until reboot. That flash is now counted as `unsynced` and kept out of the error statistics, and `max_err_us` restarts when camera mode is set (IT17e).
- **Emitter gate ownership:** the direct pulse (control thread) and the camera-mode window (strobe thread) shared one timer, so a mode change during a window could cut or stretch a pulse, or make `k_timer_status_sync` return on the other owner's expiry. The window has its own off-timer, and each owner holds the gate through its own bit; the gate is on while either holds it.
- **BIT and the IMU:** RUN_BIT called `sensor_sample_fetch` from the lane thread while the IMU thread used the same driver. BIT now uses the IMU thread's health: a good sample within the last 50 ms (IT12 still passes with the IMU removed).
- **Reset cause:** on the task-watchdog path the hardware reset flags were not cleared, so the next reset could be misreported. They are cleared on every path. (Not observable in Renode, §13.2.)
- **CAN send failures (R02):** a frame that `can_send` refused was dropped and counted only in a shell counter. Sends are retried (3 attempts), and a frame that still fails is counted in `tx_errors`, reported in STATUS (bits 13–15, saturating at 7; DBC updated) and `trainer can`. Not fault-injected: Renode cannot make sends fail.
- **Watchdog registration:** a thread that got no task-watchdog channel only logged it. It now sets BIT_F_WATCHDOG, and a build-time check ties `CONFIG_TASK_WDT_CHANNELS` to the six watched threads.
- **Debug commands:** `TRAINER_DEBUG_CMDS` defaulted to y. The default image (`prj.conf`) now leaves `trainer stall` and `trainer cfg corrupt` out; `app/debug.conf` adds them to the test image. CI builds both.
- **Tests:** IT17b's shot-pulse limit is ±100 µs like IT17 and IT17d (v0.6.0's re-plan case needed ±250 µs; with absolute tick edges it measures −19 to −35 µs). IT10c can now fail: it checks that lane time follows the reported drift over 1 s without SYNC (within 1 µs of the expected change in four runs).
