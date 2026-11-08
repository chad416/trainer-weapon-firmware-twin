# Changelog

All notable changes to this firmware. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions are `app/VERSION`. Requirement IDs (F, R, T) and test IDs (IT) refer to [SPEC.md](SPEC.md).

## [0.6.2] - 2026-11-08

Fixes from a code review of v0.6.1 (SPEC §13.7).

### Fixed
- SHOT_EVENT time is the time of the trigger sample that crossed the break point (AUTO: the cadence tick), carried in the event, instead of the time the control thread handled it. The camera-mode shot frame, the IMU metrics window and the shot log use the same time. In Renode, timestamps move earlier by less than the new `path_us` field of the SHOT log line: 55–57 µs for trigger breaks, 38–39 µs for AUTO ticks.
- Camera mode: a shot before lane time is synchronised no longer enters the pulse timing statistics with the whole timestamp as its error. It is counted as `unsynced` (`trainer strobe`), and `max_err_us` restarts when camera mode is set.
- Emitter gate: the camera-mode window has its own off-timer, and the direct pulse and the window hold the gate separately, so a mode change during a pulse cannot cut or stretch it.
- BIT no longer reads the IMU from the lane thread while the IMU thread uses the driver; it checks for a good IMU sample within the last 50 ms.
- Reset cause: the hardware reset flags are also cleared after a task-watchdog reset, so the following reset is not misreported.
- CAN TX: a failed send is retried up to 3 attempts; a frame that still fails is counted in `tx_errors`.
- A thread that gets no task-watchdog channel sets the BIT watchdog fault bit; a build-time check ties `CONFIG_TASK_WDT_CHANNELS` to the six watched threads.
- `canlink_send` rejects payloads longer than 8 bytes.

### Changed
- STATUS carries `tx_errors` in bits 13–15 (saturating at 7). DBC `VERSION "trainer 0.6.2"`; the new signal uses bits that were zero, so older decoders are unaffected.
- `trainer can` prints `tx_retry`.
- Debug shell commands (`trainer stall`, `trainer cfg corrupt`) are off by default (`TRAINER_DEBUG_CMDS`). `app/debug.conf` enables them for the test image that `scripts/build.sh` builds; `TRAINER_IMAGE=field` builds without them.
- Trigger default thresholds are defined once (`trigger_proc.h`) for `cfg.c` and `trigger_proc.c`; build-time checks that the cfg and malfunction result codes equal the CMD_ACK status codes.
- Scripts and the Robot resource take the build directory and the firmware from environment variables (`TRAINER_BUILD`, `TRAINER_ELF`) instead of fixed paths.
- CI: pinned Python packages (`requirements.txt`), a ruff lint step, and a build of the field image.

### Tests
- New IT17e (camera-mode shot before lane time). IT17b's shot-pulse limit is ±100 µs (was ±250 µs). IT10c checks that lane time follows the reported drift. IT09b checks `tx_err=0`. IT15 checks the STOPPAGE amber blink. IT01 checks version 0.6.2. Unit `proto` checks the `tx_errors` saturation.
- 81 unit tests, 34 integration tests; the full two-weapon stress run (10,000 shots per weapon at 900 rpm) passed.

### Docs
- SPEC revision C describes the firmware as built; README, traceability; LICENSE (MIT), NOTICE, this changelog.

## [0.6.1] - 2026-11-08

### Fixed
- Camera-mode emitter pulses were 563–610 µs instead of 500 µs: the gate-off timer was a relative timeout started from the strobe thread, which Zephyr extends by one 100 µs tick. Both gate edges are switched in the timer interrupt on absolute tick edges, starting on the tick edge nearest the planned start (SPEC §13.6).
- SYNC timestamps and local time use the 64-bit cycle counter instead of the 100 µs tick clock.

### Tests
- New IT17d measures ten pulses with the LED tester (500 ± 25 µs). IT17 start limit tightened to ±100 µs.

## [0.6.0] - 2026-11-08

### Added
- Camera mode (F17) for the aim-point tracker's lane camera: with STROBE_SLOTS 2–4 and STROBE_SLOT set over CAN (session keys 13, 14), the emitter flashes 0.5 ms in the middle of the camera's exposures on the node's lane time, and the shot pulse goes into the first exposure at least 2 ms after the break. The recoil valve still fires at the break. STROBE_SLOTS = 0 (default) keeps direct mode.
- Strobe thread on the task watchdog (6 channels), the inverse of the time-sync fit, 64-bit lane time, `trainer strobe`.

### Tests
- Unit suite `strobe` (5 tests); IT17, IT17b, IT17c. 81 unit tests, 32 integration tests.

## [0.5.0] - 2026-11-06

First public version.

### Added
- Analog trigger at 1 kHz with hysteresis and squeeze metrics; safety selector; smart magazine with 30 + 1; SEMI and AUTO; dry fire; emitter and recoil-valve pulses.
- Instructor stoppages (now, at round N, random) with the tap-rack drill and late-clearance report.
- IMU metrics (muzzle rate, cant, flinch) in SHOT_DETAIL.
- CAN protocol defined in `protocol/trainer.dbc`, encoders checked against cantools vectors; ISO-TP CMD/CMD_ACK; lane time from SYNC with bounded drift.
- Configuration in flash with CRC, BIT, task watchdog with IWDG fallback, status LED, on-device shot log.
- Renode platform with the HSE clock fix, one- and two-weapon setups, SocketCAN bridge; lane console, command tool, session report and demo.
- CI: firmware build, protocol-vector check, unit tests and Renode tests.

### Tests
- 76 unit tests, 29 integration tests; the full two-weapon stress run (10,000 shots per weapon at 900 rpm) passed.

[0.6.2]: https://github.com/chad416/trainer-weapon-firmware-twin/compare/d3450d9...v0.6.2
[0.6.1]: https://github.com/chad416/trainer-weapon-firmware-twin/compare/8e7b349...d3450d9
[0.6.0]: https://github.com/chad416/trainer-weapon-firmware-twin/compare/ea7d465...8e7b349
[0.5.0]: https://github.com/chad416/trainer-weapon-firmware-twin/commit/e82bff7
