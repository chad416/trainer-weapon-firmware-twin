/*
 * Instructor-set stoppage simulation. SPEC F08.
 *
 * Modes: off, at round N of every magazine, or at random with probability p
 * per round. A stopped round does not fire; the trainee clears it with the
 * tap-rack drill. A clearance slower than the window is reported as late.
 *
 * Session settings: set over CAN (keys 9..12), not saved to flash.
 * Pure logic: unit-tested on native_sim. Randomness is passed in.
 */
#ifndef TRAINER_MALFUNCTION_H_
#define TRAINER_MALFUNCTION_H_

#include <stdbool.h>
#include <stdint.h>

enum malf_mode {
	MALF_OFF = 0,
	MALF_AT_ROUND = 1,
	MALF_RANDOM = 2,
};

enum malf_key {
	MALF_KEY_MODE = 9,
	MALF_KEY_ROUND = 10,           /* 1..100 */
	MALF_KEY_PROB_PM = 11,         /* 0..1000 per round */
	MALF_KEY_CLEAR_WINDOW_MS = 12, /* 0 = no limit, else 100..60000 */
};

struct malf {
	uint8_t mode;
	uint8_t round;
	uint16_t prob_pm;
	uint16_t clear_window_ms;
	uint16_t rounds_from_mag; /* rounds attempted since the last magazine insert */
	bool done_this_mag;       /* AT_ROUND fires once per magazine */
};

void malf_init(struct malf *m);

/* Result codes match proto_cmd_status: 0 OK, 2 BAD_KEY, 3 BAD_VALUE. */
uint8_t malf_set_key(struct malf *m, uint8_t key, uint32_t value);
uint8_t malf_get_key(const struct malf *m, uint8_t key, uint32_t *value);

void malf_on_mag_insert(struct malf *m);

/* Call for every round about to fire. True if this round is a stoppage. */
bool malf_on_round(struct malf *m, uint32_t random);

bool malf_clear_late(const struct malf *m, uint32_t clear_ms);

#endif /* TRAINER_MALFUNCTION_H_ */
