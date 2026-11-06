/* Shot emitter gate, recoil valve gate and status LED. */
#ifndef TRAINER_OUTPUTS_H_
#define TRAINER_OUTPUTS_H_

#include "weapon_sm.h"

/* Configure every output as inactive. Called once before anything can fire. */
int outputs_init(void);

/* Start one shot: emitter and valve gates high, each turned off by its own timer. */
void outputs_shot(void);

/* Show the weapon state on the status LED (see SPEC F16). */
void outputs_show_state(enum weapon_state_id state);

#endif /* TRAINER_OUTPUTS_H_ */
