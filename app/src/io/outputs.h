/* Shot emitter gate, recoil valve gate and status LED. */
#ifndef TRAINER_OUTPUTS_H_
#define TRAINER_OUTPUTS_H_

#include <stdint.h>
#include <zephyr/sys/clock.h>

#include "weapon_sm.h"

/* Configure every output as inactive. Called once before anything can fire. */
int outputs_init(void);

/* Start one shot: emitter and valve gates high, each turned off by its own timer. */
void outputs_shot(void);

/* The two halves, for camera mode (v0.6): the valve at the break, the emitter when strobed. */
void outputs_valve(void);
void outputs_emitter_pulse(uint32_t us);

/*
 * Camera mode (v0.6.1): emitter gate on at system tick on_tick and off at
 * off_tick, both edges switched in the timer interrupt, so the window is
 * exact to the tick edges. on_tick must be in the future. Blocks until the
 * gate is off; returns the cycle counter at the on edge.
 */
uint64_t outputs_emitter_window(k_ticks_t on_tick, k_ticks_t off_tick);

/* Show the weapon state on the status LED (see SPEC F16). */
void outputs_show_state(enum weapon_state_id state);

#endif /* TRAINER_OUTPUTS_H_ */
