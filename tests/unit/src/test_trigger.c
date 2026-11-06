/* SPEC F01, F02, F10 (trigger part), R04: trigger calibration, hysteresis, metrics. */
#include <zephyr/ztest.h>

#include "trigger_proc.h"

static struct trig_cal cal;
static struct trig_proc tp;

/*
 * Raw ADC value that converts back to exactly `pm` with the default 12-bit
 * calibration. Rounds up, because the conversion back to permille floors.
 */
static int32_t raw_for(uint16_t pm)
{
	return (((int32_t)pm * 4095) + 999) / 1000;
}

static enum trig_event feed(uint16_t pm, uint32_t t_ms)
{
	return trig_update(&tp, raw_for(pm), t_ms * 1000U);
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	trig_cal_default(&cal);
	trig_init(&tp, &cal);
}

ZTEST_SUITE(trigger, NULL, NULL, before, NULL, NULL);

ZTEST(trigger, test_travel_conversion_and_clamp)
{
	zassert_equal(trig_travel_pm(&cal, 0), 0);
	zassert_equal(trig_travel_pm(&cal, 4095), 1000);
	zassert_within(trig_travel_pm(&cal, 2048), 500, 1);
	zassert_equal(trig_travel_pm(&cal, -100), 0, "below rest clamps to 0");
	zassert_equal(trig_travel_pm(&cal, 5000), 1000, "above full clamps to 1000");
}

ZTEST(trigger, test_inverted_sensor)
{
	cal.rest_raw = 4095;
	cal.full_raw = 0;
	zassert_equal(trig_travel_pm(&cal, 4095), 0);
	zassert_equal(trig_travel_pm(&cal, 0), 1000);
}

ZTEST(trigger, test_calibration_validation)
{
	zassert_true(trig_cal_valid(&cal));

	struct trig_cal bad = cal;

	bad.reset_pm = bad.break_pm;
	zassert_false(trig_cal_valid(&bad), "reset must be below break");

	bad = cal;
	bad.full_raw = bad.rest_raw;
	zassert_false(trig_cal_valid(&bad), "zero span");

	bad = cal;
	bad.wall_pm = bad.takeup_pm;
	zassert_false(trig_cal_valid(&bad), "take-up must be below wall");
}

ZTEST(trigger, test_held_at_power_up_does_not_fire)
{
	zassert_equal(feed(1000, 0), TRIG_EV_NONE, "held trigger at boot must not break");
	zassert_equal(feed(1000, 1), TRIG_EV_NONE);
	zassert_equal(feed(0, 2), TRIG_EV_NONE, "release without a break is not a reset");
	zassert_equal(feed(800, 3), TRIG_EV_BREAK, "armed after one release");
}

ZTEST(trigger, test_hysteresis)
{
	zassert_equal(feed(0, 0), TRIG_EV_NONE);
	zassert_equal(feed(750, 1), TRIG_EV_BREAK);
	zassert_equal(feed(800, 2), TRIG_EV_NONE, "holding is not a second break");
	zassert_equal(feed(600, 3), TRIG_EV_NONE, "above reset: still not re-armed");
	zassert_equal(feed(800, 4), TRIG_EV_NONE, "no break without a reset");
	zassert_equal(feed(400, 5), TRIG_EV_RESET);
	zassert_equal(feed(400, 6), TRIG_EV_NONE, "reset is reported once");
	zassert_equal(feed(800, 7), TRIG_EV_BREAK);
}

ZTEST(trigger, test_exact_thresholds)
{
	(void)feed(0, 0);
	zassert_equal(feed(699, 1), TRIG_EV_NONE, "just below break");
	zassert_equal(feed(700, 2), TRIG_EV_BREAK, "break is inclusive");
	zassert_equal(feed(500, 3), TRIG_EV_NONE, "reset is exclusive");
	zassert_equal(feed(499, 4), TRIG_EV_RESET);
}

ZTEST(trigger, test_squeeze_metrics)
{
	(void)feed(0, 0);
	(void)feed(150, 10); /* first movement at 10 ms */
	(void)feed(450, 20); /* wall at 20 ms */
	zassert_equal(feed(750, 40), TRIG_EV_BREAK);

	const struct trig_metrics *m = trig_last_metrics(&tp);

	zassert_equal(m->takeup_ms, 10);
	zassert_equal(m->squeeze_ms, 20);
	zassert_equal(m->jerk_pm_per_ms, 30, "fastest segment: 300 pm in 10 ms");
}

ZTEST(trigger, test_slap_in_one_sample)
{
	(void)feed(0, 0);
	zassert_equal(feed(1000, 1), TRIG_EV_BREAK);

	const struct trig_metrics *m = trig_last_metrics(&tp);

	zassert_equal(m->takeup_ms, 0);
	zassert_equal(m->squeeze_ms, 0);
	zassert_equal(m->jerk_pm_per_ms, 1000);
}

ZTEST(trigger, test_time_wraparound)
{
	uint32_t t0 = UINT32_MAX - 5000U; /* 5 ms before the 32-bit us counter wraps */

	(void)trig_update(&tp, raw_for(0), t0);
	(void)trig_update(&tp, raw_for(150), t0 + 1000U);
	(void)trig_update(&tp, raw_for(450), t0 + 4000U);
	zassert_equal(trig_update(&tp, raw_for(750), t0 + 9000U), TRIG_EV_BREAK);
	zassert_equal(trig_last_metrics(&tp)->takeup_ms, 3);
	zassert_equal(trig_last_metrics(&tp)->squeeze_ms, 5);
}
