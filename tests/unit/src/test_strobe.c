/*
 * SPEC F17 (v0.6): emitter strobing on lane time. Aim pulses in the weapon's
 * slot, the shot pulse in the first exposure >= 2 ms after the break,
 * replacing that frame's aim pulse; late shots; configuration limits.
 */
#include <zephyr/ztest.h>

#include "strobe.h"
#include "timesync.h"

ZTEST_SUITE(strobe, NULL, NULL, NULL, NULL, NULL);

ZTEST(strobe, test_config)
{
	zassert_true(strobe_cfg_ok(0, 0), "direct mode");
	zassert_true(strobe_cfg_ok(2, 1));
	zassert_true(strobe_cfg_ok(4, 3));
	zassert_false(strobe_cfg_ok(1, 0), "one slot: no dark frame for the tracker's background");
	zassert_false(strobe_cfg_ok(5, 0));
	zassert_false(strobe_cfg_ok(2, 2));
	zassert_false(strobe_cfg_ok(0, 4));

	struct strobe_cfg off = {0, 0};
	struct strobe_pulse p;

	zassert_false(strobe_next(&off, 0, STROBE_NO_SHOT, &p), "direct mode has no schedule");
}

ZTEST(strobe, test_shot_frame_rule)
{
	/* The same rule as the tracker (aimtrk/match.h). */
	zassert_equal(strobe_pulse_start(3), 34750);
	zassert_equal(strobe_shot_frame(0), 0);
	zassert_equal(strobe_shot_frame(2750), 0, "exactly the margin");
	zassert_equal(strobe_shot_frame(2751), 1);
	zassert_equal(strobe_shot_frame(123456789), 12346);
	zassert_equal(strobe_shot_frame(-7751), -1);
}

ZTEST(strobe, test_aim_pulses_in_slot)
{
	struct strobe_cfg c = {.n_slots = 3, .slot = 2};
	struct strobe_pulse p;
	int64_t t = 1000000;

	for (int i = 0; i < 10; i++) {
		zassert_true(strobe_next(&c, t, STROBE_NO_SHOT, &p));
		zassert_equal(p.kind, STROBE_AIM);
		zassert_equal(p.k % 3, 2, "slot");
		zassert_equal(p.start_us, (p.k * 10000) + 4750);
		zassert_true(p.start_us >= t);
		zassert_true((p.start_us - t) < 30000, "the next one, not a later one");
		zassert_false(p.late);
		t = p.start_us + 1;
	}
	/* Exactly at a pulse start: that pulse. */
	zassert_true(strobe_next(&c, 24750, STROBE_NO_SHOT, &p));
	zassert_equal(p.k, 2);
	/* Negative lane times (before the lane's zero) work too. */
	zassert_true(strobe_next(&c, -15000, STROBE_NO_SHOT, &p));
	zassert_equal(p.k, -1);
}

ZTEST(strobe, test_shot_pulse)
{
	struct strobe_cfg c = {.n_slots = 2, .slot = 0};
	struct strobe_pulse p;

	/* Break at 101.5 ms: shot frame 10 (pulse 104.75 ms), our aim slot: the shot replaces the aim. */
	int64_t k = strobe_shot_frame(101500);

	zassert_equal(k, 10);
	zassert_true(strobe_next(&c, 101800, k, &p));
	zassert_equal(p.kind, STROBE_SHOT);
	zassert_equal(p.k, 10);
	zassert_false(p.late);

	/* Break at 111.5 ms: shot frame 11, the other slot; it comes before our next aim (12). */
	k = strobe_shot_frame(111500);
	zassert_equal(k, 11);
	zassert_true(strobe_next(&c, 111800, k, &p));
	zassert_equal(p.kind, STROBE_SHOT);
	zassert_equal(p.k, 11);

	/* A shot frame further away than our next aim pulse: the aim pulse comes first. */
	zassert_true(strobe_next(&c, 100000, 13, &p));
	zassert_equal(p.kind, STROBE_AIM);
	zassert_equal(p.k, 10);

	/* Late: the shot frame's pulse has already started; the next exposure, flagged. */
	zassert_true(strobe_next(&c, 105000, 10, &p));
	zassert_equal(p.kind, STROBE_SHOT);
	zassert_equal(p.k, 11);
	zassert_true(p.late);
}

/* The inverse of the SYNC fit, used to time the pulses (S1 v0.6). */
ZTEST(strobe, test_timesync_inverse)
{
	struct timesync ts;

	ts_init(&ts);
	zassert_equal(ts_lane_time64(&ts, 1234), 1234);
	zassert_equal(ts_local_time(&ts, 1234), 1234U);
	/* Local clock 100 ppm fast, lane offset +5 s, across the 32-bit wrap of the lane clock. */
	for (int i = 0; i < 40; i++) {
		uint64_t local = 1000000ULL + (uint64_t)(i * 10001);
		int64_t lane = 4294967296LL - 200000 + ((int64_t)i * 10000);

		ts_add(&ts, local, (uint32_t)lane);
	}
	int64_t lane = ts_lane_time64(&ts, 1000000ULL + 39 * 10001);

	zassert_within(lane, 4294967296LL + 190000, 2, "unwrapped: %lld", (long long)lane);
	for (int64_t l = 4294967296LL; l < 4294967296LL + 500000; l += 7919) {
		uint64_t local = ts_local_time(&ts, l);

		zassert_within(ts_lane_time64(&ts, local), l, 1, "round trip at %lld", (long long)l);
	}
}
