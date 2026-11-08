/*
 * Emitter strobing on lane time for the lane camera (v0.6, SPEC F17).
 *
 * The lane camera exposes frame k from lane time k * 10 ms + 4.5 ms for 1 ms.
 * A short emitter pulse at a random time would land in an exposure only 15 %
 * of the time, so in camera mode the weapon puts its pulses in the middle of
 * the exposures, on its own estimate of lane time (F12):
 *   - aim pulse: 0.5 ms in exposure k when k mod n_slots = slot;
 *   - shot pulse: at a trigger break at lane time t, in the first exposure whose
 *     pulse starts at least STROBE_MARGIN_US after t (the fire path takes up to
 *     1.5 ms, T01), replacing the weapon's aim pulse in that frame.
 * The tracker applies the same rule to the SHOT_EVENT's t to find the frame.
 * n_slots = 0 is direct mode: the emitter fires at the break, as in v0.5.
 *
 * Pure logic: unit-tested on native_sim.
 */
#ifndef TRAINER_STROBE_H_
#define TRAINER_STROBE_H_

#include <stdbool.h>
#include <stdint.h>

#define STROBE_FRAME_US       10000
#define STROBE_PULSE_US       500
#define STROBE_PULSE_START_US 4750 /* exposure 4.5..5.5 ms, pulse in the middle */
#define STROBE_MARGIN_US      2000
#define STROBE_MAX_SLOTS      4
#define STROBE_NO_SHOT        INT64_MIN

enum strobe_kind {
	STROBE_AIM = 1,
	STROBE_SHOT = 2,
};

struct strobe_cfg {
	uint8_t n_slots; /* 0: direct mode; 2..STROBE_MAX_SLOTS: camera mode */
	uint8_t slot;    /* 0..n_slots-1 */
};

struct strobe_pulse {
	int64_t k;        /* exposure */
	int64_t start_us; /* lane time the pulse starts */
	uint8_t kind;     /* enum strobe_kind */
	bool late;        /* a shot pulse moved to a later exposure: its own had already started */
};

/* At least two slots (one weapon flashes in every other frame); 0 turns strobing off. */
bool strobe_cfg_ok(uint8_t n_slots, uint8_t slot);

/* Exposure that carries the shot pulse of a break at lane time t_break_us. */
int64_t strobe_shot_frame(int64_t t_break_us);

/* Lane time the pulse of exposure k starts. */
int64_t strobe_pulse_start(int64_t k);

/*
 * The next pulse that starts at or after lane time from_us. shot_k is the
 * pending shot frame, or STROBE_NO_SHOT. False in direct mode.
 */
bool strobe_next(const struct strobe_cfg *c, int64_t from_us, int64_t shot_k, struct strobe_pulse *p);

#endif /* TRAINER_STROBE_H_ */
