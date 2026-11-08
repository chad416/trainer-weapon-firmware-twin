#include "trigger_proc.h"

#include <string.h>

#define TRAVEL_FULL_PM 1000

static uint16_t ms_between(uint32_t from_us, uint32_t to_us)
{
	uint32_t ms = (to_us - from_us) / 1000U; /* unsigned: wrap-safe */

	return (ms > UINT16_MAX) ? UINT16_MAX : (uint16_t)ms;
}

void trig_cal_default(struct trig_cal *cal)
{
	cal->rest_raw = 0;
	cal->full_raw = 4095; /* 12-bit ADC (board devicetree: zephyr,resolution = 12) */
	cal->takeup_pm = TRIG_DEFAULT_TAKEUP_PM;
	cal->wall_pm = TRIG_DEFAULT_WALL_PM;
	cal->break_pm = TRIG_DEFAULT_BREAK_PM;
	cal->reset_pm = TRIG_DEFAULT_RESET_PM;
}

bool trig_cal_valid(const struct trig_cal *cal)
{
	return (cal->full_raw != cal->rest_raw) && (cal->takeup_pm < cal->wall_pm) &&
	       (cal->wall_pm < cal->break_pm) && (cal->reset_pm < cal->break_pm) &&
	       (cal->break_pm <= TRAVEL_FULL_PM);
}

uint16_t trig_travel_pm(const struct trig_cal *cal, int32_t raw)
{
	int64_t span = (int64_t)cal->full_raw - cal->rest_raw;
	int64_t pm = (((int64_t)raw - cal->rest_raw) * TRAVEL_FULL_PM) / span;

	if (pm < 0) {
		pm = 0;
	} else if (pm > TRAVEL_FULL_PM) {
		pm = TRAVEL_FULL_PM;
	}

	return (uint16_t)pm;
}

void trig_init(struct trig_proc *tp, const struct trig_cal *cal)
{
	(void)memset(tp, 0, sizeof(*tp));
	tp->cal = *cal;
}

static void record_rate(struct trig_proc *tp, uint16_t travel, uint32_t t_us)
{
	uint16_t rate = 0U;

	if (tp->have_prev && (travel > tp->prev_travel_pm)) {
		uint32_t dt_us = t_us - tp->prev_t_us;
		uint32_t delta = (uint32_t)travel - tp->prev_travel_pm;

		if (dt_us == 0U) {
			dt_us = 1U;
		}
		uint32_t r = (delta * 1000U) / dt_us;

		rate = (r > UINT16_MAX) ? UINT16_MAX : (uint16_t)r;
	}

	tp->rate[tp->rate_idx] = rate;
	tp->rate_idx = (uint8_t)((tp->rate_idx + 1U) % TRIG_JERK_WINDOW);
}

static uint16_t max_rate(const struct trig_proc *tp)
{
	uint16_t m = 0U;

	for (uint32_t i = 0U; i < TRIG_JERK_WINDOW; i++) {
		if (tp->rate[i] > m) {
			m = tp->rate[i];
		}
	}
	return m;
}

enum trig_event trig_update(struct trig_proc *tp, int32_t raw, uint32_t t_us)
{
	const struct trig_cal *c = &tp->cal;
	uint16_t travel = trig_travel_pm(c, raw);
	enum trig_event ev = TRIG_EV_NONE;

	record_rate(tp, travel, t_us);

	/* Movement markers for the squeeze metrics (cleared at rest). */
	if (travel < c->takeup_pm) {
		tp->past_move = false;
		tp->past_wall = false;
	} else {
		if (!tp->past_move) {
			tp->past_move = true;
			tp->t_move_us = t_us;
		}
		if ((travel >= c->wall_pm) && !tp->past_wall) {
			tp->past_wall = true;
			tp->t_wall_us = t_us;
		}
	}

	if (travel < c->reset_pm) {
		if (tp->broken) {
			ev = TRIG_EV_RESET;
			tp->broken = false;
		}
		tp->armed = true;
	} else if (tp->armed && (travel >= c->break_pm)) {
		tp->armed = false;
		tp->broken = true;
		ev = TRIG_EV_BREAK;

		/* A sample can jump past several markers at once. */
		if (!tp->past_move) {
			tp->past_move = true;
			tp->t_move_us = t_us;
		}
		if (!tp->past_wall) {
			tp->past_wall = true;
			tp->t_wall_us = t_us;
		}
		tp->last.takeup_ms = ms_between(tp->t_move_us, tp->t_wall_us);
		tp->last.squeeze_ms = ms_between(tp->t_wall_us, t_us);
		tp->last.jerk_pm_per_ms = max_rate(tp);
	}

	tp->prev_travel_pm = travel;
	tp->prev_t_us = t_us;
	tp->have_prev = true;
	tp->travel_pm = travel;

	return ev;
}

const struct trig_metrics *trig_last_metrics(const struct trig_proc *tp)
{
	return &tp->last;
}
