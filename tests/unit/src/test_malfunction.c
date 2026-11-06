/* SPEC F08: stoppage injection modes and clearance window. */
#include <zephyr/ztest.h>

#include "malfunction.h"

static struct malf m;

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	malf_init(&m);
}

ZTEST_SUITE(malfunction, NULL, NULL, before, NULL, NULL);

ZTEST(malfunction, test_off_never_stops)
{
	malf_on_mag_insert(&m);
	for (int i = 0; i < 200; i++) {
		zassert_false(malf_on_round(&m, (uint32_t)i));
	}
}

ZTEST(malfunction, test_at_round_n_once_per_magazine)
{
	zassert_equal(malf_set_key(&m, MALF_KEY_MODE, MALF_AT_ROUND), 0);
	zassert_equal(malf_set_key(&m, MALF_KEY_ROUND, 5), 0);

	for (int mag = 0; mag < 2; mag++) {
		malf_on_mag_insert(&m);
		for (int r = 1; r <= 30; r++) {
			zassert_equal(malf_on_round(&m, 0U), r == 5, "mag %d round %d", mag, r);
		}
	}
}

ZTEST(malfunction, test_random_extremes)
{
	zassert_equal(malf_set_key(&m, MALF_KEY_MODE, MALF_RANDOM), 0);

	zassert_equal(malf_set_key(&m, MALF_KEY_PROB_PM, 1000), 0);
	for (uint32_t i = 0; i < 100U; i++) {
		zassert_true(malf_on_round(&m, i * 2654435761U), "p = 1000 always stops");
	}
	zassert_equal(malf_set_key(&m, MALF_KEY_PROB_PM, 0), 0);
	for (uint32_t i = 0; i < 100U; i++) {
		zassert_false(malf_on_round(&m, i * 2654435761U), "p = 0 never stops");
	}
}

ZTEST(malfunction, test_random_rate)
{
	uint32_t x = 12345U;
	int stops = 0;

	(void)malf_set_key(&m, MALF_KEY_MODE, MALF_RANDOM);
	(void)malf_set_key(&m, MALF_KEY_PROB_PM, 100); /* 10 % */
	for (int i = 0; i < 10000; i++) {
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		stops += malf_on_round(&m, x) ? 1 : 0;
	}
	zassert_within(stops, 1000, 100, "about 10 %% of 10000 rounds, got %d", stops);
}

ZTEST(malfunction, test_key_validation)
{
	uint32_t v;

	zassert_equal(malf_set_key(&m, MALF_KEY_MODE, 3), 3);
	zassert_equal(malf_set_key(&m, MALF_KEY_ROUND, 0), 3);
	zassert_equal(malf_set_key(&m, MALF_KEY_ROUND, 101), 3);
	zassert_equal(malf_set_key(&m, MALF_KEY_PROB_PM, 1001), 3);
	zassert_equal(malf_set_key(&m, MALF_KEY_CLEAR_WINDOW_MS, 50), 3);
	zassert_equal(malf_set_key(&m, MALF_KEY_CLEAR_WINDOW_MS, 0), 0, "0 = no limit");
	zassert_equal(malf_set_key(&m, 99, 1), 2);
	zassert_equal(malf_get_key(&m, 99, &v), 2);
	zassert_equal(malf_get_key(&m, MALF_KEY_ROUND, &v), 0);
	zassert_equal(v, 5, "default round");
}

ZTEST(malfunction, test_clear_window)
{
	zassert_false(malf_clear_late(&m, 60000U), "no window: never late");
	(void)malf_set_key(&m, MALF_KEY_CLEAR_WINDOW_MS, 2000);
	zassert_false(malf_clear_late(&m, 2000U));
	zassert_true(malf_clear_late(&m, 2001U));
}
