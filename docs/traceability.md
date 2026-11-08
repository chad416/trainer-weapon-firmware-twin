# Requirement traceability

Every requirement in [SPEC.md](../SPEC.md) §4 with the tests that cover it and its status.
Unit tests are ztest suites in `tests/unit/src/` (run on `native_sim`). IT tests are Robot Framework tests in `tests/robot/trainer.robot` (run in Renode).
Results are from the final run on 6 Nov 2026, firmware v0.5.0: 76/76 unit tests, all 29 integration tests passing in two consecutive runs (58/58), and the full IT09b stress run (10,000 shots per weapon, 17 min) on the same build.

**Status key:**
- **Met:** implemented and tested.
- **Partial:** part of the requirement is implemented or tested; the note says which part.
- **Not implemented:** open.

## Functional

| ID | Requirement (short) | Tests | Status | Note |
|---|---|---|---|---|
| F01 | Trigger sampled at 1 kHz, per-unit calibration in settings | unit `trigger` (conversion, clamp, inverted sensor, calibration validation); IT03 | Partial | 1 kHz verified (1001 samples in 1002 ms). Break and reset thresholds are stored settings (keys 6, 7). The rest and full-travel ADC points come from the ADC range, not a per-unit calibration. |
| F02 | Break threshold, re-arm only below reset (hysteresis) | unit `trigger` (`test_hysteresis`, `test_exact_thresholds`); IT03 | Met | |
| F03 | Powers up SAFE; SW1 cycles SAFE → SEMI → AUTO | unit `weapon` (`test_powers_up_safe`, `test_selector_cycle`, `test_trigger_blocked_in_safe`); IT01, IT01b | Met | |
| F04 | Magazine capacity, rack chambers, 30 + 1 | unit `rounds` (9 cases); IT02 | Met | |
| F05 | Fire conditions, emitter and valve pulses, auto-feed | unit `weapon`; IT02, IT14 | Met | 2 ms emitter and 20 ms valve pulses measured with LED testers |
| F06 | SEMI one shot per break; AUTO 600–900 rpm | unit `weapon` (`test_semi_one_shot_per_break`, `test_auto_cadence`, `test_auto_runs_dry_and_stops_cadence`); IT03, IT04 | Met | |
| F07 | Dry fire on an empty chamber | unit `weapon` (`test_empty_and_loaded_are_dry_fire`); IT05 | Met | |
| F08 | Instructor stoppages (now, at round N, probability p), tap-rack drill, clearance time, window | unit `malfunction` (6), `weapon` (`test_stoppage_drill`); IT06, IT06a, IT06b, IT06c | Met | Stoppage settings are session-only (keys 9–12), not saved to flash |
| F09 | SHOT_EVENT for every shot | unit `proto`; IT02, IT09a, IT09b | Met | |
| F10 | SHOT_DETAIL: trigger and IMU metrics | unit `imu` (7), `proto`; IT07, IT07b, IT07c, IT07d | Met | One classic CAN frame with saturating fields (take-up and squeeze in 4 ms steps) |
| F11 | STATUS at 10 Hz | unit `proto`; IT01 | Met | The 100 ms period is visible in the demo `candump`; no test asserts the rate |
| F12 | Lane time synchronisation | unit `timesync` (8); IT10b, IT10c | Met | Absolute accuracy is proven in unit tests (≤ 50 µs at ±200 ppm), because Renode delays test frames (SPEC §13.3) |
| F13 | Configuration over CAN and shell, saved with CRC | unit `cfg` (7); IT08, IT08b, IT08c, IT14b, IT17c | Met | Strobe settings (keys 13, 14, v0.6) are session-only |
| F14 | BIT at boot and on request; critical faults block fire | IT01 (clean BIT_RESULT), IT12 (IMU missing → FAULT, red LED, no fire, RUN_BIT) | Partial | The IMU path is fault-injected. The ADC, outputs, trigger-not-at-rest, CAN, flash and watchdog bits are implemented but not fault-injected in a test. |
| F15 | Shot log: ring buffer of the last 1000 shots, dump over shell (CAN in v2) | unit `shotlog` (5: wrap keeps the last 1000 in order, overwrite count, signed fields); IT16 (`trainer log n` matches the SHOT_EVENT lane times; `trainer log clear`) | Met | RAM only (16 KB, 16-byte records), lost on reset as specified. Segmented CAN download stays v2. |
| F16 | Status LED patterns | IT01 (blue), IT12 (red), IT15 (blink, green) | Met | |
| F17 | Camera mode (v0.6): emitter strobed in the lane camera's exposures on lane time; shot pulse in the first exposure ≥ 2 ms after the break | unit `strobe` (5: slots, shot frame rule, shot replacing the aim pulse, late shot, inverse of the SYNC fit); IT17, IT17b, IT17c | Met | Pulses 68–168 µs after plan in Renode (100 µs tick), inside the ±250 µs the exposure allows. The tracker side is tested in S3 (aim-point-tracker). |

## Robustness

| ID | Requirement (short) | Tests | Status | Note |
|---|---|---|---|---|
| R01 | Task watchdog on the threads; reset with the reason logged | IT11 (control), IT11b (trigger) | Met | The reason is logged before the reset. After the reset, Renode reports PIN, because it clears RAM and the RCC flags (SPEC §13.2). |
| R02 | Bus-off recovery; TX overflow counted, never silent | IT09b (`tx_overflow=0` at 900 rpm on both weapons) | Partial | Bus-off can't be injected in Renode (IT13 untested). Recovery is Zephyr's automatic mode. |
| R03 | Corrupt settings → defaults + fault bit | unit `cfg` (`test_corruption_is_detected`); IT08b | Met | |
| R04 | A reset never fires a shot | unit `trigger` (`test_held_at_power_up_does_not_fire`), `weapon` (`test_held_trigger_when_leaving_safe_does_not_fire`); IT01 (no output pulses at boot) | Met | |

## Timing (virtual time in Renode; SPEC §9.4)

| ID | Requirement | Tests | Status | Note |
|---|---|---|---|---|
| T01 | Break detected ≤ 1 ms after the threshold crossing | IT10 | Met | Covered by IT10's combined bound below |
| T02 | Outputs ≤ 200 µs after detection | IT10 | Partial | Measured together with T01: emitter on ≤ 1.5 ms after the pot crosses break. Not measured separately. |
| T03 | SHOT_EVENT ≤ 2 ms after break | IT10 | Met | |
| T04 | AUTO cadence within ±2 % | IT04 | Met | 80 ± 1 ms at 750 rpm (±1.25 %) |
| T05 | Two weapons at 900 rpm, 10 000 shots each, no lost frames | IT09b with `STRESS_MAGS:100` | Met | Node 2's stream is verified frame by frame (one CAN tester per hub in Renode). Node 1 is verified by its shot counter and TX overflow counter. |

## Not tested in simulation

- **IT13 bus-off:** Renode can't inject CAN bus errors.
- **Silicon timing:** Renode isn't cycle-accurate. The same binary has to run on hardware for real latencies.
