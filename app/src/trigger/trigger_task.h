/* Trigger sampling thread: ADC at CONFIG_TRAINER_TRIGGER_SAMPLE_HZ, feeds trigger_proc. */
#ifndef TRAINER_TRIGGER_TASK_H_
#define TRAINER_TRIGGER_TASK_H_

#include <stdbool.h>
#include <stdint.h>

#include "trigger_proc.h"

struct trigger_snapshot {
	int32_t raw;
	uint16_t travel_pm;
	bool armed;
	struct trig_metrics last;
	uint32_t samples;
	uint32_t adc_errors;
	uint32_t read_us_max; /* longest single ADC read */
};

/* Set up the ADC channel and start sampling. Returns 0 or a negative errno. */
int trigger_task_start(void);

void trigger_get_snapshot(struct trigger_snapshot *out);

/* Debug (IT11): make the trigger thread stop feeding its watchdog channel. */
void trigger_debug_stall(void);

#endif /* TRAINER_TRIGGER_TASK_H_ */
