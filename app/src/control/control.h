/* Control thread: owns the weapon state machine, the event queue and the cadence timer. */
#ifndef TRAINER_CONTROL_H_
#define TRAINER_CONTROL_H_

#include <stdint.h>

#include "shotlog.h"
#include "weapon_sm.h"

struct control_snapshot {
	enum weapon_state_id state;
	enum weapon_selector sel;
	struct rounds rounds;
	uint32_t shots;
	uint32_t last_clear_ms;
	uint32_t dropped_events;
};

/* Start the control thread. Outputs must already be initialised. */
void control_start(void);

/* Queue an event, timestamped now. Safe to call from ISRs. Returns 0 or -ENOMSG if full. */
int control_post(enum weapon_event_type type);

/*
 * Queue an event that happened at local time t_local_us (local_clock.h), for
 * inputs sampled before they are posted: a trigger break carries the time of
 * the ADC sample that crossed the break point.
 */
int control_post_at(enum weapon_event_type type, uint64_t t_local_us);

/* Consistent copy of the weapon status, for the shell and STATUS frames. */
void control_get_snapshot(struct control_snapshot *out);

/* Debug (IT11): make the control thread stop feeding its watchdog channel. */
void control_debug_stall(void);

/* On-device shot log (SPEC F15): last 1000 shots, RAM only. */
void control_shotlog_stats(uint16_t *count, uint32_t *total);
bool control_shotlog_get(uint16_t age, struct shot_record *out); /* age 0 = newest */
void control_shotlog_clear(void);

/* Session (not saved) stoppage settings, keys 9..12. Return proto_cmd_status values. */
uint8_t control_session_set(uint8_t key, uint32_t value);
uint8_t control_session_get(uint8_t key, uint32_t *value);

#endif /* TRAINER_CONTROL_H_ */
