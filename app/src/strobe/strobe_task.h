/*
 * The strobe thread: fires the emitter pulses of camera mode (strobe.h) at
 * the local times that correspond to their lane times (inverse of the SYNC
 * fit). Configured with two session-only CMD keys (SPEC F13, F17):
 *   13 STROBE_SLOTS  0 = direct mode (default), 2..4 = camera mode
 *   14 STROBE_SLOT   0..3, below STROBE_SLOTS
 */
#ifndef TRAINER_STROBE_TASK_H_
#define TRAINER_STROBE_TASK_H_

#include <stdbool.h>
#include <stdint.h>

#define STROBE_KEY_SLOTS 13U
#define STROBE_KEY_SLOT  14U

struct strobe_stats {
	uint8_t n_slots, slot;
	bool synced;
	uint32_t pulses, aim, shots, late;
	uint32_t unsynced;        /* shot flashes fired at once: camera mode without lane time */
	uint8_t last_kind;
	int64_t last_k;           /* exposure of the last pulse */
	int64_t last_lane_us;     /* lane time it fired */
	uint64_t last_local_us;
	int64_t last_err_us;      /* fired - planned, lane time */
	int64_t max_err_us;       /* worst |last_err_us| since camera mode was last set */
	int64_t last_shot_k;      /* shot frame of the last break */
	int64_t last_shot_fired_k;
	int64_t last_shot_lane_us;
};

void strobe_start(void);

/* Camera mode configured (shots then go through strobe_shot). */
bool strobe_active(void);

/* A trigger break at lane time t (unwrapped): schedule the shot pulse. */
void strobe_shot(int64_t t_break_lane_us);

/* Session keys; results are proto_cmd_status values. */
bool strobe_is_key(uint8_t key);
uint8_t strobe_session_set(uint8_t key, uint32_t value);
uint8_t strobe_session_get(uint8_t key, uint32_t *value);

void strobe_get_stats(struct strobe_stats *out);

#endif /* TRAINER_STROBE_TASK_H_ */
