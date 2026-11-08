/*
 * Analog trigger processing: calibration, break/reset detection with
 * hysteresis, and squeeze metrics.
 *
 * Pure logic, no Zephyr device access, so it can be unit-tested on native_sim.
 * Travel is expressed in permille (0 = rest, 1000 = full travel).
 */
#ifndef TRAINER_TRIGGER_PROC_H_
#define TRAINER_TRIGGER_PROC_H_

#include <stdbool.h>
#include <stdint.h>

/* Default thresholds (permille of travel); cfg.c uses the same values. */
#define TRIG_DEFAULT_TAKEUP_PM 100U
#define TRIG_DEFAULT_WALL_PM   400U
#define TRIG_DEFAULT_BREAK_PM  700U
#define TRIG_DEFAULT_RESET_PM  500U

/* Number of samples kept for the jerk metric (50 ms at 1 kHz). */
#define TRIG_JERK_WINDOW 50U

struct trig_cal {
	int32_t rest_raw;   /* ADC reading with the trigger at rest */
	int32_t full_raw;   /* ADC reading at full travel */
	uint16_t takeup_pm; /* start of pull (finger on the trigger) */
	uint16_t wall_pm;   /* end of take-up slack */
	uint16_t break_pm;  /* shot is released here */
	uint16_t reset_pm;  /* trigger must return below this to re-arm */
};

struct trig_metrics {
	uint16_t takeup_ms;      /* first movement -> wall */
	uint16_t squeeze_ms;     /* wall -> break */
	uint16_t jerk_pm_per_ms; /* fastest travel increase in the 50 ms before break */
};

enum trig_event {
	TRIG_EV_NONE = 0,
	TRIG_EV_BREAK, /* travel crossed break_pm while armed */
	TRIG_EV_RESET, /* travel fell below reset_pm after a break */
};

struct trig_proc {
	struct trig_cal cal;
	struct trig_metrics last;
	uint16_t travel_pm;
	uint16_t prev_travel_pm;
	uint32_t prev_t_us;
	uint32_t t_move_us;
	uint32_t t_wall_us;
	bool have_prev;
	bool armed;    /* below reset since boot or since the last break */
	bool broken;   /* a break happened and reset has not yet been seen */
	bool past_move;
	bool past_wall;
	uint16_t rate[TRIG_JERK_WINDOW]; /* permille per ms, per sample */
	uint8_t rate_idx;
};

/* Default calibration for a 12-bit ADC reading 0..4095 over full travel. */
void trig_cal_default(struct trig_cal *cal);

/* True if the calibration is usable (ordered thresholds, non-zero span). */
bool trig_cal_valid(const struct trig_cal *cal);

/* Convert a raw ADC reading to travel in permille, clamped to 0..1000. */
uint16_t trig_travel_pm(const struct trig_cal *cal, int32_t raw);

/*
 * Initialise. The trigger starts disarmed: it must be seen below reset_pm
 * once before a break can fire. This stops a trigger held during power-up
 * from firing a shot.
 */
void trig_init(struct trig_proc *tp, const struct trig_cal *cal);

/* Feed one ADC sample taken at time t_us. Returns the event it caused. */
enum trig_event trig_update(struct trig_proc *tp, int32_t raw, uint32_t t_us);

/* Metrics of the most recent break. */
const struct trig_metrics *trig_last_metrics(const struct trig_proc *tp);

#endif /* TRAINER_TRIGGER_PROC_H_ */
