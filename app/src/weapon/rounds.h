/*
 * Magazine and chamber bookkeeping for the trainer weapon.
 *
 * Pure logic, no Zephyr device access, so it can be unit-tested on native_sim.
 * Model (v1): every magazine insert is a full magazine of `capacity` rounds,
 * removing the magazine takes its remaining rounds with it, and a chambered
 * round stays in the weapon when the magazine is removed ("30 + 1").
 */
#ifndef TRAINER_ROUNDS_H_
#define TRAINER_ROUNDS_H_

#include <stdbool.h>
#include <stdint.h>

struct rounds {
	uint8_t capacity;   /* rounds in a full magazine */
	uint8_t mag_rounds; /* rounds left in the inserted magazine */
	bool mag_in;        /* magazine inserted */
	bool chambered;     /* round in the chamber */
};

/* Reset to "no magazine, empty chamber" with the given magazine capacity. */
void rounds_init(struct rounds *r, uint8_t capacity);

/* Insert a full magazine. No effect if a magazine is already inserted. */
void rounds_insert_mag(struct rounds *r);

/* Remove the magazine. The chambered round (if any) stays. */
void rounds_remove_mag(struct rounds *r);

/*
 * Work the charging handle: eject the chambered round (if any), then feed
 * a new round from the magazine if one is available.
 * Returns true if a round was ejected.
 */
bool rounds_rack(struct rounds *r);

/*
 * Fire the chambered round and auto-feed the next one from the magazine.
 * Returns true if a round was fired, false if the chamber was empty (dry fire).
 */
bool rounds_fire(struct rounds *r);

/* Total rounds in the weapon: magazine plus chamber. */
uint8_t rounds_total(const struct rounds *r);

#endif /* TRAINER_ROUNDS_H_ */
