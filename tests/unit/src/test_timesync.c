/* SPEC F12, T03: lane time synchronisation (offset, drift, jitter, wrap, restart). */
#include <stdlib.h>
#include <zephyr/ztest.h>

#include "timesync.h"

static struct timesync ts;

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	ts_init(&ts);
}

ZTEST_SUITE(timesync, NULL, NULL, before, NULL, NULL);

/* Lane clock as a function of the node's local clock. */
static int64_t lane_of(uint64_t local_us, int64_t offset_us, double drift_ppm)
{
	return (int64_t)((double)local_us * (1.0 + (drift_ppm * 1e-6))) + offset_us;
}

/* Feed SYNC every 10 ms with +-jitter us of reception jitter; return worst error afterwards. */
static int64_t run(int64_t offset_us, double drift_ppm, int jitter_us, int n_sync)
{
	uint32_t seed = 1U;
	uint64_t local = 1000000U;

	for (int i = 0; i < n_sync; i++) {
		seed = (seed * 1103515245U) + 12345U;
		int j = (jitter_us > 0) ? ((int)(seed >> 16) % (2 * jitter_us + 1)) - jitter_us : 0;

		ts_add(&ts, local + (uint64_t)(int64_t)j,
		       (uint32_t)lane_of(local, offset_us, drift_ppm));
		local += 10000U;
	}

	/* Predict lane time over the next 100 ms (ten missed SYNC periods), worst error. */
	int64_t worst = 0;

	for (uint64_t t = local; t < local + 100000U; t += 5000U) {
		int64_t err = (int64_t)(uint32_t)lane_of(t, offset_us, drift_ppm) -
			      (int64_t)ts_lane_time(&ts, t);

		/* compare on the 32-bit circle */
		err = (int64_t)(int32_t)(uint32_t)err;
		if (llabs(err) > worst) {
			worst = llabs(err);
		}
	}
	return worst;
}

ZTEST(timesync, test_unsynced_is_local)
{
	zassert_false(ts.synced);
	zassert_equal(ts_lane_time(&ts, 123456U), 123456U);
}

ZTEST(timesync, test_pure_offset)
{
	int64_t worst = run(5000000, 0.0, 0, 20);

	zassert_true(ts.synced);
	zassert_true(worst <= 1, "worst error %lld us", worst);
}

ZTEST(timesync, test_drift_plus_200ppm_with_jitter)
{
	int64_t worst = run(-2500000, 200.0, 5, 100);

	zassert_true(worst <= 50, "worst error %lld us (SPEC: <= 50)", worst);
	zassert_within(ts_drift_ppm(&ts), 200.0, 20.0);
}

ZTEST(timesync, test_drift_minus_200ppm_with_jitter)
{
	int64_t worst = run(777777, -200.0, 5, 100);

	zassert_true(worst <= 50, "worst error %lld us (SPEC: <= 50)", worst);
	zassert_within(ts_drift_ppm(&ts), -200.0, 20.0);
}

ZTEST(timesync, test_heavy_jitter_keeps_drift_bounded)
{
	/* +-3 ms reception jitter (under the 10 ms restart threshold): the slope must stay sane. */
	(void)run(1000000, 0.0, 3000, 200);
	zassert_true(ts_drift_ppm(&ts) <= 500.0 + 1e-6, "drift %d ppm", (int)ts_drift_ppm(&ts));
	zassert_true(ts_drift_ppm(&ts) >= -500.0 - 1e-6, "drift %d ppm", (int)ts_drift_ppm(&ts));
}

ZTEST(timesync, test_single_sync_is_pure_offset)
{
	ts_add(&ts, 1000000U, 6000000U);
	zassert_true(ts.synced);
	zassert_equal(ts_lane_time(&ts, 1234567U), 6234567U);
	zassert_equal(ts_offset_us(&ts, 1234567U), 5000000);
}

ZTEST(timesync, test_lane_clock_wrap)
{
	/* Lane clock starts 50 ms before its 32-bit wrap. */
	int64_t offset = (int64_t)UINT32_MAX - 50000 - 1000000;
	int64_t worst = run(offset, 0.0, 0, 20);

	zassert_true(worst <= 1, "worst error %lld us", worst);
	zassert_equal(ts.resets, 0, "a wrap is not a restart");
}

ZTEST(timesync, test_lane_restart_resets_the_fit)
{
	(void)run(5000000, 0.0, 0, 20);
	zassert_equal(ts.resets, 0);

	/* Lane restarts: its clock jumps back to near zero. */
	uint64_t local = 2000000U;

	for (int i = 0; i < 5; i++) {
		ts_add(&ts, local, (uint32_t)(1000 + (i * 10000)));
		local += 10000U;
	}
	zassert_equal(ts.resets, 1);
	zassert_within((int64_t)ts_lane_time(&ts, local), 1000 + (5 * 10000), 2);
}
