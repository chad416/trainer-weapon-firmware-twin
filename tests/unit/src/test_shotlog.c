/* SPEC F15: on-device shot log (ring buffer of the last 1000 shots). */
#include <zephyr/ztest.h>

#include "shotlog.h"

static struct shotlog slog;

static struct shot_record rec(uint16_t n)
{
	return (struct shot_record){.shot_no = n, .t_lane_us = 1000U * n, .rounds_left = 7,
				    .flinch_ddeg = -3};
}

static void add_n(uint16_t first, uint16_t n)
{
	for (uint16_t i = 0; i < n; i++) {
		struct shot_record r = rec((uint16_t)(first + i));

		shotlog_add(&slog, &r);
	}
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	shotlog_init(&slog);
}

ZTEST_SUITE(shotlog, NULL, NULL, before, NULL, NULL);

ZTEST(shotlog, test_record_is_16_bytes)
{
	zassert_equal(sizeof(struct shot_record), 16, "1000 records = 16 KB of RAM");
}

ZTEST(shotlog, test_empty)
{
	struct shot_record r;

	zassert_equal(shotlog_count(&slog), 0);
	zassert_equal(shotlog_total(&slog), 0);
	zassert_false(shotlog_get(&slog, 0, &r));
}

ZTEST(shotlog, test_newest_first_by_age)
{
	struct shot_record r;

	add_n(1, 3);
	zassert_equal(shotlog_count(&slog), 3);
	zassert_true(shotlog_get(&slog, 0, &r));
	zassert_equal(r.shot_no, 3, "age 0 is the newest");
	zassert_true(shotlog_get(&slog, 2, &r));
	zassert_equal(r.shot_no, 1, "age count-1 is the oldest");
	zassert_equal(r.t_lane_us, 1000U);
	zassert_equal(r.flinch_ddeg, -3, "signed fields survive");
	zassert_false(shotlog_get(&slog, 3, &r));
}

ZTEST(shotlog, test_full_buffer_keeps_last_1000)
{
	struct shot_record r;

	add_n(1, 1000);
	zassert_equal(shotlog_count(&slog), 1000);
	zassert_true(shotlog_get(&slog, 999, &r));
	zassert_equal(r.shot_no, 1);

	add_n(1001, 5); /* overwrites shots 1..5 */
	zassert_equal(shotlog_count(&slog), 1000);
	zassert_equal(shotlog_total(&slog), 1005);
	zassert_true(shotlog_get(&slog, 0, &r));
	zassert_equal(r.shot_no, 1005);
	zassert_true(shotlog_get(&slog, 999, &r));
	zassert_equal(r.shot_no, 6, "oldest held after overwriting 5");
}

ZTEST(shotlog, test_every_record_in_order_after_wrap)
{
	struct shot_record r;

	add_n(1, 2500);
	for (uint16_t age = 0; age < 1000; age++) {
		zassert_true(shotlog_get(&slog, age, &r));
		zassert_equal(r.shot_no, 2500 - age, "age %u", age);
	}
}
