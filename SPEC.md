# S1 — Trainer-Weapon Firmware on a Digital Twin

Specification, revision A (6 Nov 2026)

Production-style embedded C firmware for an instrumented training weapon. It runs on Zephyr RTOS on an emulated STM32H753 board (Antmicro's STM32H7 Renode Reference Platform) inside the Renode simulator, with automated integration tests and CI. No physical hardware is needed. The same binary is built for a real board, so it can be flashed later without changes.

> Keep company names out of this repository, the README and the commit messages. It is a public portfolio project.

---

## 1. Why this project

| Junior electronics & firmware profile | Where S1 shows it |
|---|---|
| C firmware for microcontrollers and embedded systems | Zephyr application in C: drivers through devicetree, threads, timers, ADC, I2C, CAN, flash, watchdog |
| Bring-up, testing and debugging | BIT, shell diagnostics, logging, fault injection in the simulator |
| Integration with the software team | CAN protocol defined in a DBC file, Python lane-console tool |
| Sensors, CAN, USB | LSM6DSO IMU, analog trigger, FDCAN; USB is a v2 option |
| Reliability for real-world use | Watchdog, CAN bus-off recovery, persistent configuration with CRC, requirement-traced tests |

It also takes the firmware half of the parked hardware spec (`HW_PROJECTS/01_instrumented-trainer-module/SPEC.md`, §7–§8) and proves it without a board.

### What the product does (from public sources)
Reference trainer weapons:
- match the real weapon's mass and balance
- have a non-contact trigger sensor
- use "smart" magazines that count rounds and give CO2 recoil
- talk to the lane over a wireless link
- support instructor-injected malfunctions that the trainee has to clear
- produce after-action data: aim movement, firing rhythm, reloads, malfunction handling

S1 models all of these. The link from the weapon to the lane is modelled as **CAN**, standing in for the belt-worn interface module's link (§12 covers the radio).

---

## 2. Scope

### In scope
- One Zephyr application, `trainer_fw`, for board `stm32h7_renode_reference_board`. Upstream Zephyr may rename it `..._platform`; check `boards/antmicro/` in your tree.
- Renode setups for one weapon, and for two weapons plus a lane bus.
- Unit tests (ztest on `native_sim`) and integration tests (Robot Framework + Renode).
- GitHub Actions CI.
- A DBC file and a Python lane console.

### Out of scope
- Real hardware
- Camera aim tracking (that is S3)
- Wireless radio (v2)
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
| Tap (malfunction drill) / BIT request on long press | SW4 (PG10) | Button Press/Release |
| Shot emitter output (laser/IR gate) | GPIO PE2 (free pin) | Simulated LED `emitterGate` in `sim/trainer_board.repl`, read by the LED tester |
| Recoil valve output (CO2 solenoid gate) | GPIO PE3 (free pin) | Simulated LED `valveGate`, read by the LED tester |
| Status indication | RGB LED (PD13–15) | Read by the LED tester |
| Aim, cant, flinch | LSM6DSO IMU on I2C | Data replayed from RESD files |
| Lane link | FDCAN1, classic CAN 500 kbit/s | Renode CAN hub (plus a SocketCAN bridge on Linux) |
| Configuration and shot log | Internal flash (settings/NVS) | Survives `machine Reset` (verify, §13) |
| Debug | USART2 console + Zephyr shell | Renode UART analyzer |

Put pin and peripheral choices in an **app devicetree overlay** with aliases (`trigger-adc`, `sel-btn`, `emitter-led`, and so on). A future real board then only needs a new overlay, not code changes.

*Week 1 change:* the board's green LEDs sit on TIM3 PWM and have no LED model in Renode, so the emitter and valve gates moved to free GPIOs PE2/PE3. The trainer's Renode platform (`sim/trainer_board.repl`) attaches LED models to those pins.

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
| F09 | Send a SHOT_EVENT on CAN for every shot. |
| F10 | Send SHOT_DETAIL with trigger metrics (take-up time, squeeze duration, jerk score) and IMU metrics (mean angular rate over 300 ms before break, cant angle at break, flinch = pitch change in the last 50 ms). |
| F11 | Send STATUS at 10 Hz: state, selector, magazine, rounds, chambered, stoppage, fault bits, uptime. |
| F12 | Lane time synchronisation: a SYNC frame from the lane carries lane time. The node estimates offset and drift and stamps events in lane time. |
| F13 | Configuration over CAN and the shell: node ID, capacity, cadence, thresholds, pulse widths, malfunction settings. Stored with settings/NVS and checked by CRC. |
| F14 | BIT at boot and on request: IMU ID and self-test, trigger ADC within the calibrated rest band, config CRC, CAN controller state, flash read/write. Send BIT_RESULT. Critical faults block fire. |
| F15 | Shot log: ring buffer of the last 1000 shot records. Dumped through the shell, and through segmented CAN transfer in v2. |
| F16 | Status LED: SAFE = blue, READY = green, STOPPAGE = amber blink, FAULT = red, EMPTY = slow green blink. |

### 4.2 Robustness
| ID | Requirement |
|---|---|
| R01 | A task watchdog covers the trigger, control and CAN threads. A stalled thread causes a reset with the reason logged. |
| R02 | CAN bus-off: automatic recovery with back-off. TX queue overflow increments a counter reported in STATUS, and is never silently dropped. |
| R03 | Corrupt settings fall back to defaults, set a fault bit, and are logged. |
| R04 | A reset never fires a shot. Outputs default to off in the devicetree, and the state machine always starts in SAFE. |

### 4.3 Timing (measured in emulated virtual time, see §9.4)
| ID | Requirement |
|---|---|
| T01 | Trigger break detected within one sample period (≤ 1 ms after the ADC crosses the threshold). |
| T02 | Emitter and valve outputs set ≤ 200 µs after detection. |
| T03 | SHOT_EVENT on the bus ≤ 2 ms after break. |
| T04 | AUTO cadence within ±2 % of the set rpm. |
| T05 | Two weapons at 900 rpm on one bus: no lost frames over 10 000 shots. |

---

## 5. Firmware architecture

```
               ┌──────────── Zephyr kernel ────────────┐
 ADC (1 kHz) ─►│ trigger_thr (prio 2)                  │
               │   calib → travel → break/reset detect │
               │        │ zbus: trigger_chan            │
 Buttons ─────►│ input  ▼                              │
 (gpio-keys)   │ control_thr (prio 3)                  │
               │   SMF weapon state machine            │──► emitter / valve LEDs (k_timer one-shots)
               │   rounds, malfunction, cadence timer  │──► status LED
               │        │ zbus: shot_chan, status_chan  │
 IMU (I2C) ───►│ imu_thr (prio 5) ring buffer 500 ms   │
               │        ▼                              │
               │ can_thr (prio 4) TX queue, RX dispatch│◄─► FDCAN1
               │ timesync, cmd handler, BIT, settings  │
               │ shell + logging (low prio)            │
               └───────────────────────────────────────┘
```

Design choices:
- **Zephyr State Machine Framework (SMF)** for the weapon states. It is hierarchical: ARMED contains READY and FIRING.
- **zbus** for publishing and subscribing between modules. No module calls another module's internals.
- **Devicetree aliases and Kconfig** for every hardware dependency and tunable.
- Fire outputs are switched in `control_thr` immediately after the zbus notification. Pulse width comes from a `k_timer` one-shot.
- The pure logic modules (`trigger_proc`, `weapon_sm`, `rounds`, `timesync`, `metrics`) have **no Zephyr device calls**. That makes them unit-testable on `native_sim`.

### 5.1 Weapon state machine
States:
- `SAFE`
- `ARMED`, containing `EMPTY`, `LOADED` (magazine in, nothing chambered), `READY` (chambered) and `FIRING`
- `STOPPAGE`
- `FAULT`

Rules:
- `SAFE` is entered from anywhere when the selector is on SAFE. Rounds are preserved.
- `STOPPAGE` is entered from `FIRING` when a malfunction triggers. It is left only by the correct tap-then-rack sequence.
- `FAULT` is entered on a critical BIT fault and is left only by a BIT pass.
- Every transition is logged and published on `status_chan`.

### 5.2 Modules
| Module | File(s) | Responsibility |
|---|---|---|
| trigger_proc | `src/trigger/` | Calibration, filtering, threshold detection with hysteresis, squeeze metrics |
| weapon_sm | `src/weapon/` | SMF states, fire decisions, cadence |
| rounds | `src/weapon/rounds.c` | Magazine and chamber logic (30 + 1) |
| malfunction | `src/weapon/malfunction.c` | Injection modes, clearance drill and timing |
| outputs | `src/io/outputs.c` | Emitter, valve and status LED pulses |
| imu | `src/imu/` | Zephyr sensor API for the LSM6DSO, 500 ms ring buffer, pre-shot metrics |
| proto | `src/proto/` | CAN encode/decode generated from the DBC, or hand-written with a test against the DBC |
| canlink | `src/can/` | TX queue, RX filters, bus-off handling |
| timesync | `src/timesync/` | Offset/drift estimate from SYNC |
| bit | `src/bit/` | Self-tests and fault bitmask |
| cfg | `src/cfg/` | Settings handlers, defaults, CRC |
| shell_cmds | `src/shell/` | `trainer status`, `trainer cfg`, `trainer log`, `trainer bit`, `trainer inject` |

---

## 6. CAN protocol

- **Bus and IDs:** 11-bit IDs at 500 kbit/s, laid out as `function (4 bits) | node ID (7 bits)`.
- **Definition:** the source of truth is `protocol/trainer.dbc`. The Python side reads it with `cantools`.

| Frame | Function | Dir | Rate | Signals |
|---|---|---|---|---|
| SYNC | 0x1 | lane → all | 100 Hz | ISO-TP single frame: PCI 0x04, lane_time_us (u32) |
| SHOT_EVENT | 0x2 | node → lane | per shot | shot_no (u16), t_lane_us (u32), mode (2 bits), rounds_left (u8), chambered (1 bit) |
| SHOT_DETAIL | 0x3 | node → lane | per shot | shot_no (u16), takeup_ms (u8), squeeze_ms (u8), jerk (u8), muzzle_rate deg/s (u8), cant deg (i8), flinch 0.1 deg (i8). One classic frame, saturating, rather than CAN-FD. |
| STATUS | 0x4 | node → lane | 10 Hz | state, selector, mag_in, rounds, chambered, stoppage, faults (u16), tx_ovf (u8) |
| EVENT | 0x5 | node → lane | on event | type (dry fire, mag in/out, rack, stoppage start/cleared, safety change), value, t_lane_us |
| BIT_RESULT | 0x6 | node → lane | on event | fault_mask (u16), detail |
| CMD | 0x7 | lane → node | on demand | opcode (set/get cfg, BIT, inject stoppage, reset counters), key, value |
| CMD_ACK | 0x8 | node → lane | reply | opcode, key, status, value |
| INFO | 0x9 | node → lane | on boot / request | fw version, git hash (short), build date |

Add a unit test that encodes each frame in C, decodes it with `cantools`, and compares the result. That keeps the firmware and the DBC from drifting apart.

---

## 7. Simulation setup (Renode)

### 7.1 Scripts
- `sim/trainer_board.repl`: the board's Renode description (from the Zephyr tree, Apache-2.0) with the HSE fix and the emitter/valve LED models (§13.1).
- `sim/single.resc`: one weapon. Loads the platform above and `zephyr.elf`.
- `sim/lane.resc`: two weapon machines (node IDs 1 and 2) on one CAN hub. It is based on the multinode demo in Antmicro's reference repository.
- `sim/data/*.resd`: IMU recordings (steady aim, shaky aim, flinch before break, cant), made with a small Python generator.

### 7.2 Lane side (three options, in order of preference)
1. **Linux or WSL2:** load the `vcan` module, run `machine CreateSocketCANBridge "socketcan" "vcan0"` and connect the bridge to the hub. The Python lane console and Wireshark then use `vcan0`.
2. **If WSL2's kernel lacks `vcan`:** run a small **lane-controller Zephyr app** as a third Renode machine. It sends SYNC, runs a shell for commands and prints the shot table on its UART.
3. **Tests only:** inject and inspect frames from Robot through Renode's CAN test keywords (confirm the exact keyword names in your Renode version).

---

## 8. Tooling and environment

- **Windows 11 + WSL2 Ubuntu 24.04:** Zephyr SDK, `west`, the Renode Linux package, Python 3.12 venv (`python-can`, `cantools`, `robotframework`).
- **Zephyr version:** pin it in `west.yml` (Antmicro's support targets Zephyr 4.4). Record the exact Renode version in the README.
- **Build:** `west build -b stm32h7_renode_reference_board app`
- **Run:** `west simulate` (Renode runner), or `renode sim/single.resc`
- **Integration tests:** `renode-test tests/robot/*.robot`
- **Unit tests:** `west twister -p native_sim -T tests/unit`

---

## 9. Verification

### 9.1 Unit tests (ztest on native_sim)
| Suite | Covers |
|---|---|
| trigger_proc | F01, F02: calibration maths, hysteresis edges, noise rejection, squeeze metrics against hand-computed values |
| weapon_sm | F03–F07: every legal and illegal transition, AUTO cadence counts, SAFE preserving rounds |
| rounds | F04: insert/remove/rack permutations, the 30 + 1 case, empty magazine |
| malfunction | F08: each injection mode, wrong clearance order, timeout |
| timesync | F12: synthetic offset and ±200 ppm drift, convergence within 1 s, error ≤ 50 µs |
| proto | §6: encode/decode round trip, compared with a cantools-generated fixture |
| cfg | R03: CRC failure → defaults |

Collect gcov coverage on native_sim and publish the percentage. Target ≥ 85 % on the logic modules.

### 9.2 Integration tests (Robot Framework + Renode)
| Test | Covers | Scenario |
|---|---|---|
| IT01 boot | F03, F14, F16, R04 | Boot → SAFE, blue LED, BIT_RESULT clean, INFO frame, no output pulses |
| IT02 full cycle | F04–F06, F09 | Insert magazine, rack, SEMI, 30 squeezes → 30 SHOT_EVENTs, rounds count down, EMPTY after the last |
| IT03 semi reset | F02 | Break → partial release above reset threshold → break again: only one shot |
| IT04 auto cadence | F06, T04 | AUTO, hold the trigger for 2 s at 750 rpm → 25 ± 1 shots, inter-shot intervals within ±2 % |
| IT05 dry fire | F07 | Break with an empty chamber → EVENT dry fire, outputs stay off |
| IT06 stoppage drill | F08 | Inject at round 5 → STOPPAGE, fire blocked; rack-then-tap fails; tap-then-rack clears; clearance time reported |
| IT07 IMU metrics | F10 | Replay the flinch RESD → flinch metric above threshold; steady RESD → below. *As built:* flinch ≈ 1.2° (IT07), steady ≈ 0 (IT07b), shaky raises the muzzle rate (IT07c), canted reads 10° (IT07d). |
| IT08 config persist | F13, R03 | Set capacity 20 over CMD → reset → capacity still 20; corrupt the settings area → defaults + fault bit |
| IT09 two weapons | T05, F09 | `lane.resc`: both at 900 rpm for 10 000 shots each → all shot numbers continuous, no TX overflow. *As built (IT09b):* node 2's stream is read in full while node 1 shares the bus, and both nodes' shot counters and TX overflow are checked. CI runs 3 magazines; `--variable STRESS_MAGS:100` runs the full 10 000. |
| IT10 timing | T01–T03 | Ramp the trigger past the threshold at a known virtual time and check output and frame timestamps in virtual time. *As built:* emitter on ≤ 1.5 ms after the pot crosses break, SHOT_EVENT ≤ 2 ms later (IT10); lane-time stamping (IT10b); drift bound under jitter (IT10c). |
| IT11 watchdog | R01 | Shell command (debug builds only) stalls `control_thr` → reset, reset reason logged |
| IT12 BIT fault | F14 | Start with the IMU removed from the platform → FAULT, red LED, fire blocked |
| IT13 bus-off | R02 | If the CAN model supports error injection, force bus-off and check recovery. Otherwise document it as untested in simulation. |
| IT14 shot pulses | F05, F13 | One shot pulses the emitter gate (2 ms) and the valve gate (20 ms); a valve width set over CAN applies to the next shot (added in week 1–2) |
| IT15 status LED | F16 | SAFE blue, EMPTY slow green blink, READY steady green (added in week 1) |

### 9.3 CI (GitHub Actions, ubuntu-latest)
1. Fetch Zephyr through `west` and cache it.
2. Build for the board, plus `native_sim` for the unit tests.
3. Run twister unit tests and generate coverage.
4. Run the Robot suites in Renode.
5. Upload the Robot HTML report, Renode logs and a CAN trace as artifacts.
6. Show a status badge in the README.

### 9.4 Honest limit
Renode runs instructions functionally, not cycle-accurately. The timing tests (IT10, IT04) prove **scheduling logic and ordering in virtual time**, not real STM32H7 latencies. State this in the README and the interview. Real numbers come from flashing the same firmware onto hardware later.

---

## 10. Lane console (Python)

- `lane_console.py` (connects over `vcan0`) shows:
  - a live table per weapon: state, selector, rounds, stoppage
  - a scrolling shot log with the detail metrics
  - command keys: inject stoppage, request BIT, change cadence
- `session_report.py` turns a session log into a CSV and PNG plots: shot timeline per weapon, squeeze-time histogram, flinch per shot, and clearance times.
- Optional later: the same console in C#/WPF on Windows, through a small TCP bridge.

---

## 11. Deliverables (portfolio proof)

1. Repository with a README: what it is, a one-command quick start, an architecture diagram and the honest-limit note.
2. **Real PNG screenshots** of running output:
   - Renode with the UART shell
   - Wireshark decoding CAN with the DBC
   - the Robot report
   - the lane console
   - the session-report plots
3. Requirement traceability matrix (`docs/traceability.md`): each ID → tests → result.
4. A 60-second screen recording: two weapons, a stoppage drill and the live console.
5. A green CI badge.

---

## 12. Milestones (about 8–10 h/week)

| Week | Work | Done when |
|---|---|---|
| 1 | WSL2 setup; build and run Antmicro's demo in Renode; app skeleton + overlay; trigger ADC, buttons, LEDs, shell; SMF state machine; first ztests | `trainer status` works in Renode; IT01 passes |
| 2 | CAN protocol + DBC, canlink, STATUS/SHOT_EVENT/CMD; settings + CRC; BIT; watchdog; `lane.resc` | IT02–IT05, IT08 pass |
| 3 | IMU + RESD generator + metrics; malfunction drill; timesync; remaining Robot suites; CI | All ITs pass or are documented; CI green |
| 4 | Lane console + session report; screenshots, video, README, traceability | Portfolio-ready |

### v2 (list in the README, don't build yet)
- Wireless link on a second emulated radio MCU. Check Renode's BLE/802.15.4 support for nRF52 first.
- CO2 gas budget per magazine (pressure drop per shot, cold-temperature penalty).
- Segmented shot-log download over CAN.
- MCUboot with signed images.
- Flash onto a real board.

---

## 13. Open points to verify early (week 1)

- [x] Exact Zephyr board name: `stm32h7_renode_reference_board` at the pinned Zephyr revision `c605ea46` (4.4.99, the revision Antmicro's demo uses).
- [x] Renode commands: `sysbus.adcM1S2.potentiometer SetPercentage <0-100>`; buttons `sysbus.gpioPortG.sw1..sw4 Press` / `Release`. A press must be held longer than the gpio-keys 30 ms debounce, so `PressAndRelease` does not work.
- [x] LSM6DSO RESD replay works on this platform (`FeedAccelerationSamplesFromRESD` / `FeedAngularRateSamplesFromRESD` with `CurrentVirtualTime`), and IT07–IT07d pass.
- [x] Stored settings survive a reset: the settings live in the QSPI storage partition, and its Renode memory survives `machine Reset` and firmware reboots (IT08 passes).
- [x] `modprobe vcan` works in WSL2 (kernel 6.18), so option 1 in §7.2 is available.
- [x] Renode CAN keywords: `Create CAN Tester`, `Wait For Frame With Id`, `Send ISOTP Message`, `Wait For ISOTP Message Hex`. There is no keyword for injecting bus errors, so IT13 (bus-off) stays untested in simulation; the recovery code path is the Zephyr default.
- [x] The LED tester resolves the 2 ms emitter and 20 ms valve pulses (IT07 passes).

### 13.1 Week 1 findings
- **Simulator clock bug (fixed):** Renode's STM32H7 RCC model defaults the HSE crystal to 8 MHz, but the board has 25 MHz, and the board's Renode description does not set it. With the firmware's PLL settings, Renode set SysTick to 153.6 MHz instead of 480 MHz, so firmware time ran at 0.32× virtual time and debounced buttons stopped working. Fix: `sim/trainer_board.repl` (the board description plus `rcc: hseFrequency: 25000000`). Confirmed: SysTick reads 480 MHz after boot, and 1001 trigger samples arrive in 1002 ms.
- **Logging latency:** Zephyr's deferred logger flushes once a second by default. It is set to wake on every message so test harnesses see events promptly.
- **Robot gotcha:** text starting with `name=` is read as a named argument (e.g. `state=SAFE`), and a backslash escape does not survive Renode's remote keywords. Pass it as `content=state=SAFE ...`.
- **Paths:** Zephyr builds, twister and `renode-test` all break on the space in "New CLI". The scripts bind-mount the repo at `~/trainer-src` inside WSL automatically.

### 13.2 Week 2 findings
- **CMD/CMD_ACK are ISO-TP single frames** (byte 0 = PCI). Standard diagnostic tools can talk to a node, and Renode's CAN tester can send and await them deterministically, without a host CAN interface. That keeps CI free of `vcan`.
- **DBC is the source of truth:** `protocol/gen_vectors.py` encodes sample messages with cantools into `tests/unit/src/proto_vectors.h`, and the C encoders must match byte for byte. `--check` fails CI if the header is stale.
- **Default node ID** comes from the MCU unique ID (1 + byte sum mod 127). Renode's system-flash model holds a fixed fake unique ID (it gave node 111), so the simulator scripts pin it: zeros give node 1, and `lane.resc` gives weapon 2 node 2. Variables set in a `.resc` did not reach the included script reliably, so the values are literals.
- **Renode CAN tester behaviour:** all frame IDs share one queue. A wait consumes every frame up to its match, and a live match clears the queue, so tests wait for frames in send order. In Renode 1.17, `SendFrame` also resumes a paused emulation, so a reply can arrive in a window where the wait misses it. The reply stays queued, so `Send Cmd` recovers with a short second wait. A 200-command stress run confirmed the firmware received every frame (`rx_cmd=200`, `rx_drop=0`).
- **Reset reason in simulation:** a Renode reset clears RAM (where the task-watchdog note is kept) and does not keep RCC software-reset flags, so after a watchdog or commanded reboot the simulated reset reason reads PIN. On silicon, the `__noinit` note and RCC_RSR give TASK_WATCHDOG and SOFTWARE. IT11 therefore checks the WATCHDOG log line and the boot counter.
- **Bus-off (IT13)** cannot be injected with Renode's CAN tester, so it is untested in simulation.

### 13.3 Week 3 findings
- **IMU in Renode:** the LSM6DSO model's `Default*` properties do not change the readings in the mode Zephyr uses, but `Feed*Sample` and the RESD feeds do. RESD units are micro-g for acceleration and 1e-5 rad/s for angular rate. `sim/data/gen_imu_profiles.py` builds four recordings (steady, shaky, flinch, canted) with Renode's `csv2resd`.
- **Flinch integration** uses the rectangle rule from the oldest sample inside the 50 ms window to the shot, so it can under-count by up to one sample period (about 5 ms at 208 Hz).
- **Stoppage modes** are session settings over CAN (keys 9–12, not saved to flash). The control thread turns a round attempt into a stoppage before the state machine sees it, so `weapon_sm` stays unchanged and its tests still apply. Randomness comes from xorshift32 seeded from the cycle counter: training realism, not security.
- **Time sync:** least squares over the last 32 SYNC points (about 310 ms), with the drift bounded to ±500 ppm. The bound was added after Renode's delivery jitter produced an absurd slope (see below). A jump over 10 ms restarts the fit, and 32-bit lane wraps are unwrapped. Unit tests show ≤ 50 µs error at ±200 ppm with 5 µs jitter when predicting 100 ms ahead.
- **Renode limits for timing tests:** frames sent by the CAN tester reach the node after a host-dependent delay (tens of ms of virtual time), and only one CAN tester can exist per hub. IT10b therefore uses one SYNC (pure offset) and checks the on-target conversion exactly (0 µs), IT10c checks the drift bound under that jitter, and IT09b reads one node's full stream.
- **SHOT_DETAIL** is one classic 8-byte frame with saturating fields, rather than CAN-FD, so the protocol stays readable on any CAN 2.0 tool.

### 13.4 Week 4 findings (lane console and demo)
- **Live bridge:** `sim/lane_live.resc` connects the emulated CAN hub to the host's SocketCAN `vcan0` (`machine CreateSocketCANBridge`; there is no `emulation` variant). The Python lane console (`tools/lane/`) decodes every frame with the DBC, sends SYNC every 10 ms, records a CSV, and renders the screen. `scripts/demo.sh` runs a scripted two-weapon session through Renode's monitor socket.
- **Protocol change from the first recording:** take-up and squeeze saturated at 255 ms, too short for slow aimed squeezes. SHOT_DETAIL now carries them in 4 ms steps (0–1020 ms). Firmware and DBC are v0.4.
- **Monitor latency:** a monitor client that waited for each command's output added about 0.25 s per command and distorted the scripted squeezes. Commands are now fire-and-forget, with output drained between steps.
- **Renode IMU model:** outside FIFO mode it holds the latest fed sample, and the repeat count has no effect. A scripted motion therefore lasts until the next feed.
- **Screenshot rendering:** `cairosvg` drew the wrong text in one cell of a correct SVG (an AUTO shot shown as SEMI), so all PNGs are rendered with `rsvg-convert`.
- **Video:** the scripted session is 36 s, not the planned 60 s. It is the real length of the session, not padded.
- **F15 closed in v0.5.0:** the traceability review found the on-device shot log missing. It is now a 1000-record RAM ring buffer (16-byte records, 16 KB), filled from the same data as SHOT_EVENT and SHOT_DETAIL and dumped with `trainer log [n|clear]`. Covered by unit `shotlog` and IT16. Segmented CAN download remains v2.
- **IT10b test bug:** IT10b took the shot's local time from the log timestamp, which has only ms resolution here (the µs field is always `,000`). It passed while shots landed on ms boundaries and failed with 900 µs error once they didn't. The SHOT log line now prints the exact `t_local` in µs, and IT10b checks lane time = local + offset to within 2 µs. It measures 0 µs.

---

## 14. Interview story (60 seconds)

> "I wrote the firmware for an instrumented training weapon on Zephyr: an analog trigger with hysteresis, safety selector, smart magazine with 30 + 1 logic, AUTO cadence, instructor-injected stoppages with a clearance drill, IMU flinch and cant metrics, and a CAN protocol defined in a DBC file. It runs on an emulated STM32H7 in Renode, so every requirement has an automated test. Two weapons share one bus in CI and do 10 000 shots each without losing a frame. I'm clear about what simulation can and can't prove: timing is virtual, so the next step is the same binary on real hardware."
